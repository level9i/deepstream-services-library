// level9i splish-6.3-patched (2026-09-14 valve-splitmuxsink cascade fix)
//
// XRotatedFileSinkBintr implementation. See DslXRotatedFileSinkBintr.h
// for the design rationale; see workspace/docs/proposals/
//   recording-service-phase2/xrotated-file-sink-design.md for the
// full spec and mechanism analysis.

/*
The MIT License

Copyright (c) 2019-2024, Prominence AI, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in-
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "Dsl.h"
#include "DslXRotatedFileSinkBintr.h"

#include <cstdio>
#include <cstring>

namespace DSL
{
    // Structure names for element bus messages emitted on each rotation.
    static const char* XROTATED_FILE_FRAGMENT_OPENED = "xrotatedfile-fragment-opened";
    static const char* XROTATED_FILE_FRAGMENT_CLOSED = "xrotatedfile-fragment-closed";

    // Default bounded wait for mp4mux finalise, in seconds.
    static const uint DEFAULT_FINALIZE_TIMEOUT_SEC = 5;

    // -----------------------------------------------------------------
    // EOS-arrival probe used by _finaliseChildPair. qtmux finalises its
    // output (flush samples → write moov → push EOS downstream) upon
    // receiving EOS at its sink pad. Once the EOS event reaches
    // filesink's sink pad, ALL preceding buffers — including the moov
    // backpatch — have been delivered to filesink. Only then is it safe
    // to NULL filesink; force-NULL before that races with the moov
    // write and produces truncated files (no moov, or 36-byte ftyp-
    // only shells).
    // -----------------------------------------------------------------
    struct EosArrivalCtx
    {
        GMutex mutex;
        GCond cond;
        bool seen;
    };

    static GstPadProbeReturn _eosArrivalProbeCb(GstPad* pad,
        GstPadProbeInfo* info, gpointer userData)
    {
        if (!(GST_PAD_PROBE_INFO_TYPE(info)
              & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM))
        {
            return GST_PAD_PROBE_OK;
        }
        GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
        if (GST_EVENT_TYPE(event) != GST_EVENT_EOS)
        {
            return GST_PAD_PROBE_OK;
        }
        EosArrivalCtx* ctx = static_cast<EosArrivalCtx*>(userData);
        g_mutex_lock(&ctx->mutex);
        ctx->seen = true;
        g_cond_broadcast(&ctx->cond);
        g_mutex_unlock(&ctx->mutex);
        return GST_PAD_PROBE_OK;
    }

    // -------------------------------------------------------------------
    // Ctor / dtor
    // -------------------------------------------------------------------

    XRotatedFileSinkBintr::XRotatedFileSinkBintr(const char* name,
        const char* filePathTemplate, uint bitrate, uint interval)
        : EncodeSinkBintr(name, DSL_CODEC_H264, bitrate, interval)
        , m_filePathTemplate(filePathTemplate)
        , m_currentIndex(0)
        , m_maxSizeTimeNs(0)
        , m_rotationTimerId(0)
        , m_finalizeTimeoutSec(DEFAULT_FINALIZE_TIMEOUT_SEC)
        , m_rotationProbeId(0)
        , m_padBlocked(false)
        , m_isRecording(true)
        , m_stoppedInitially(false)
    {
        LOG_FUNC();

        // B4-PIVOT: create the post-parser queue + valve. Both live
        // permanently between parser and the container/filesink pair.
        // The queue is leaky-downstream to absorb brief buffer bursts
        // during rotation/stop/start without back-pressuring the
        // encoder. The valve is the gate: drop=true halts data flow to
        // the container without touching parser's src pad state.
        char queueName[256];
        char valveName[256];
        std::snprintf(queueName, sizeof(queueName),
            "%s-post-parser-queue", name);
        std::snprintf(valveName, sizeof(valveName), "%s-valve", name);
        m_pPostParserQueue = DSL_ELEMENT_NEW("queue", queueName);
        m_pValve = DSL_ELEMENT_NEW("valve", valveName);

        // leaky=2 = downstream (drops oldest buffers when full).
        m_pPostParserQueue->SetAttribute("leaky", 2);
        m_pPostParserQueue->SetAttribute("max-size-buffers", 30);
        m_pPostParserQueue->SetAttribute("max-size-bytes", 0);
        m_pPostParserQueue->SetAttribute("max-size-time", 0);

        // Start valve open — LinkAll flips it closed if stoppedInitially.
        m_pValve->SetAttribute("drop", false);

        AddChild(m_pPostParserQueue);
        AddChild(m_pValve);

        // Prime the current fragment path for the initial fragment.
        m_currentFragmentPath = _renderFragmentPath(m_currentIndex);

        // Create the initial qtmux + filesink pair for fragment 0.
        if (!_createChildPair(m_currentFragmentPath, m_pContainer, m_pFileSink))
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << name
                << "' failed to create initial mp4mux/filesink pair");
            throw;
        }

        LOG_INFO("");
        LOG_INFO("Initial property values for XRotatedFileSinkBintr '" << name << "'");
        LOG_INFO("  file-path-template : " << filePathTemplate);
        LOG_INFO("  initial-fragment   : " << m_currentFragmentPath);
        LOG_INFO("  codec              : " << m_codec);
        LOG_INFO("  container          : qtmux (mp4, fixed for v1)");
        LOG_INFO("  gating             : valve+leaky-queue (B4-PIVOT)");
        if (m_bitrate)
        {
            LOG_INFO("  bitrate            : " << m_bitrate);
        }
        else
        {
            LOG_INFO("  bitrate            : " << m_defaultBitrate);
        }
        LOG_INFO("  interval           : " << m_interval);
        LOG_INFO("  max-size-time (ns) : " << m_maxSizeTimeNs
            << "  (0 = auto-rotation disabled)");
        LOG_INFO("  finalize-timeout   : " << m_finalizeTimeoutSec << " s");
    }

    XRotatedFileSinkBintr::~XRotatedFileSinkBintr()
    {
        LOG_FUNC();

        // Cancel the auto-rotation timer if running.
        if (m_rotationTimerId)
        {
            g_source_remove(m_rotationTimerId);
            m_rotationTimerId = 0;
        }

        if (IsLinked())
        {
            UnlinkAll();
        }
    }

    // -------------------------------------------------------------------
    // LinkAll / UnlinkAll
    // -------------------------------------------------------------------

    bool XRotatedFileSinkBintr::LinkAll()
    {
        LOG_FUNC();

        if (m_isLinked)
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' is already linked");
            return false;
        }

        // B4-PIVOT: encoder chain + post-parser queue + valve are ALWAYS
        // linked. The container/filesink pair is linked downstream of
        // the valve. Recording/stopped state is a valve.drop flag,
        // not a topology change.
        if (!m_pQueue->LinkToSink(m_pTransform) or
            !m_pTransform->LinkToSink(m_pCapsFilter) or
            !m_pCapsFilter->LinkToSink(m_pEncoder) or
            !m_pEncoder->LinkToSink(m_pParser) or
            !m_pParser->LinkToSink(m_pPostParserQueue) or
            !m_pPostParserQueue->LinkToSink(m_pValve))
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' failed to link encoder chain + queue/valve");
            return false;
        }

        // Link valve → container → filesink (the swap-on-rotation pair).
        if (!m_pValve->LinkToSink(m_pContainer) or
            !m_pContainer->LinkToSink(m_pFileSink))
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' failed to link initial container/filesink");
            return false;
        }

        // Apply deploy-time state via the valve.
        if (m_stoppedInitially)
        {
            _setValveDrop(true);
            m_isRecording = false;
        }
        else
        {
            _setValveDrop(false);
            m_isRecording = true;
        }

        m_isLinked = true;

        // Fragment 0's OPENED event: for RECORDING deploy, post here at
        // LinkAll time — no explicit Start() call is expected, so this
        // is the sink's only "you have a live fragment now" signal.
        // For STOPPED-INITIAL deploy, DO NOT post yet — Start() will
        // post when the valve opens, matching the "start() is when the
        // fragment becomes live" semantic. Posting from both LinkAll
        // and Start would produce a duplicate open for fragment 0.
        if (!m_stoppedInitially)
        {
            _postFragmentMessage(XROTATED_FILE_FRAGMENT_OPENED,
                m_currentFragmentPath);
        }
        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' linked (" << (m_stoppedInitially ? "STOPPED" : "RECORDING")
            << "); initial fragment '" << m_currentFragmentPath << "'");

        return true;
    }

    void XRotatedFileSinkBintr::UnlinkAll()
    {
        LOG_FUNC();

        if (!m_isLinked)
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' is not linked");
            return;
        }

        // B4-PIVOT: unlink the full always-linked chain plus the
        // container→filesink pair if present.
        if (m_pContainer)
        {
            m_pContainer->UnlinkFromSink();
        }
        if (m_pValve)
        {
            m_pValve->UnlinkFromSink();
        }
        if (m_pPostParserQueue)
        {
            m_pPostParserQueue->UnlinkFromSink();
        }
        m_pParser->UnlinkFromSink();
        m_pEncoder->UnlinkFromSink();
        m_pCapsFilter->UnlinkFromSink();
        m_pTransform->UnlinkFromSink();
        m_pQueue->UnlinkFromSink();
        m_isLinked = false;
    }

    // -------------------------------------------------------------------
    // Auto-rotation timer
    // -------------------------------------------------------------------

    uint64_t XRotatedFileSinkBintr::GetMaxSizeTime()
    {
        LOG_FUNC();
        return m_maxSizeTimeNs;
    }

    bool XRotatedFileSinkBintr::SetMaxSizeTime(uint64_t maxSizeTimeNs)
    {
        LOG_FUNC();

        // Remove existing timer if present.
        if (m_rotationTimerId)
        {
            g_source_remove(m_rotationTimerId);
            m_rotationTimerId = 0;
        }

        m_maxSizeTimeNs = maxSizeTimeNs;

        if (maxSizeTimeNs == 0)
        {
            LOG_INFO("XRotatedFileSinkBintr '" << GetName()
                << "' auto-rotation disabled");
            return true;
        }

        // GLib timers use ms — convert from ns with round-to-nearest.
        guint intervalMs = (guint)((maxSizeTimeNs + 500000) / 1000000);
        if (intervalMs == 0)
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' rejected max-size-time = " << maxSizeTimeNs
                << " ns — sub-millisecond periods are not supported");
            m_maxSizeTimeNs = 0;
            return false;
        }

        m_rotationTimerId = g_timeout_add(intervalMs,
            (GSourceFunc)_autoRotateTimerCb, (gpointer)this);

        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' auto-rotation armed at " << intervalMs << " ms");
        return true;
    }

    // -------------------------------------------------------------------
    // RotateNow — the heart of the class
    // -------------------------------------------------------------------

    bool XRotatedFileSinkBintr::RotateNow()
    {
        LOG_FUNC();

        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_rotationMutex);

        if (!m_isLinked)
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' RotateNow called while unlinked — no-op");
            return false;
        }

        if (!m_isRecording)
        {
            LOG_INFO("XRotatedFileSinkBintr '" << GetName()
                << "' RotateNow called while stopped — no-op");
            return true;
        }

        // B4-PIVOT (2026-09-15): valve-gated rotation.
        // The valve upstream of the container acts as the "block probe"
        // — closing it halts data flow to the container so we can
        // safely finalise + swap the container/filesink pair. Parser is
        // permanently linked to (queue→)valve; only valve's DOWNSTREAM
        // side is touched during rotation. No more parser-peer swap
        // dance.
        uint nextIndex = m_currentIndex + 1;
        std::string nextPath = _renderFragmentPath(nextIndex);

        DSL_ELEMENT_PTR oldContainer = m_pContainer;
        DSL_ELEMENT_PTR oldFileSink = m_pFileSink;
        std::string closedPath = m_currentFragmentPath;

        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' rotation: closing valve, finalising '" << closedPath
            << "', opening fresh pair '" << nextPath << "'");

        // 1) Close the valve — no new data reaches container.
        _setValveDrop(true);

        // 2) Finalise the closed fragment while valve→oldContainer→
        //    oldFileSink is still fully linked. Sends EOS to
        //    oldContainer; waits for EOS at oldFileSink's sink pad
        //    (moov write complete); NULLs both.
        _finaliseChildPair(oldContainer, oldFileSink);
        _postFragmentMessage(XROTATED_FILE_FRAGMENT_CLOSED, closedPath);

        // 3) Unlink and drop the old pair. valve's src pad is now
        //    unpeered, which is fine — valve.drop=true means no
        //    buffers are being pushed downstream anyway.
        m_pValve->UnlinkFromSink();
        RemoveChild(oldContainer);
        RemoveChild(oldFileSink);

        // 4) Create the fresh pair and link valve → newContainer →
        //    newFileSink. This is a stable link — valve's src pad
        //    doesn't have the caps-commitment problem that h264parse
        //    had, so gst_element_link cleanly requests a fresh qtmux
        //    sink_%u.
        DSL_ELEMENT_PTR newContainer;
        DSL_ELEMENT_PTR newFileSink;
        if (!_createChildPair(nextPath, newContainer, newFileSink))
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' RotateNow failed to create fresh pair");
            return false;
        }
        if (!m_pValve->LinkToSink(newContainer) or
            !newContainer->LinkToSink(newFileSink))
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' RotateNow failed to link fresh pair to valve");
            return false;
        }

        // 5) Sync the fresh pair up to PLAYING so it's ready before
        //    we open the valve.
        if (!gst_element_sync_state_with_parent(newContainer->GetGstElement()) or
            !gst_element_sync_state_with_parent(newFileSink->GetGstElement()))
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' sync_state_with_parent returned failure on fresh pair");
        }

        // 6) Update state and open the valve — buffers flow into the
        //    fresh mp4mux.
        m_pContainer = newContainer;
        m_pFileSink = newFileSink;
        m_currentIndex = nextIndex;
        m_currentFragmentPath = nextPath;
        _setValveDrop(false);

        _postFragmentMessage(XROTATED_FILE_FRAGMENT_OPENED,
            m_currentFragmentPath);
        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' rotated: closed '" << closedPath
            << "' → opened '" << m_currentFragmentPath << "'");

        return true;
    }

    // -------------------------------------------------------------------
    // GetCurrentFragmentPath
    // -------------------------------------------------------------------

    const char* XRotatedFileSinkBintr::GetCurrentFragmentPath()
    {
        LOG_FUNC();
        return m_currentFragmentPath.c_str();
    }

    // -------------------------------------------------------------------
    // SetState override — send EOS to inner mp4mux BEFORE the parent
    // walks the child bin to NULL. This is the reliability property
    // we're paying for.
    // -------------------------------------------------------------------

    bool XRotatedFileSinkBintr::SetState(GstState state, uint timeoutNs)
    {
        LOG_FUNC();

        // Only intercept transitions that end the current fragment.
        // NULL and READY both close the file. PAUSED is a legal
        // intermediate state we don't want to finalise on. If we are
        // in the stopped state there is no active container/filesink
        // to EOS — the fakesink transitions cleanly on its own.
        if ((state == GST_STATE_NULL or state == GST_STATE_READY)
            and m_isLinked
            and m_isRecording
            and m_pContainer
            and m_pFileSink)
        {
            LOG_INFO("XRotatedFileSinkBintr '" << GetName()
                << "' finalising current fragment before state change to "
                << gst_element_state_get_name(state));

            // Cancel any auto-rotation timer.
            if (m_rotationTimerId)
            {
                g_source_remove(m_rotationTimerId);
                m_rotationTimerId = 0;
            }

            // Send EOS + bounded wait. We don't set the pair to NULL
            // here — the parent state-change will do that below. This
            // gives mp4mux a chance to write moov cleanly.
            GstPad* pSinkPad = gst_element_get_static_pad(
                m_pContainer->GetGstElement(), "sink");
            if (pSinkPad)
            {
                gst_pad_send_event(pSinkPad, gst_event_new_eos());
                gst_object_unref(pSinkPad);

                // Bounded wait for filesink to enter READY.
                gint64 endTime = g_get_monotonic_time()
                    + (G_TIME_SPAN_SECOND * m_finalizeTimeoutSec);
                GstState curState;
                GstState pendingState;
                while (g_get_monotonic_time() < endTime)
                {
                    gst_element_get_state(m_pFileSink->GetGstElement(),
                        &curState, &pendingState, 100 * GST_MSECOND);
                    if (curState <= GST_STATE_READY)
                    {
                        break;
                    }
                }
                _postFragmentMessage(XROTATED_FILE_FRAGMENT_CLOSED,
                    m_currentFragmentPath);
                LOG_INFO("XRotatedFileSinkBintr '" << GetName()
                    << "' finalised fragment '" << m_currentFragmentPath
                    << "' ahead of state change");
            }
        }

        // Delegate to parent for the actual state change.
        return Bintr::SetState(state, timeoutNs);
    }

    // -------------------------------------------------------------------
    // IsRecording / Stop / Start / SetStoppedInitially
    // -------------------------------------------------------------------

    bool XRotatedFileSinkBintr::IsRecording()
    {
        LOG_FUNC();
        return m_isRecording;
    }

    bool XRotatedFileSinkBintr::SetStoppedInitially(bool stoppedInitially)
    {
        LOG_FUNC();

        if (m_isLinked)
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' SetStoppedInitially called after LinkAll — must be"
                << " called before deploy");
            return false;
        }
        m_stoppedInitially = stoppedInitially;
        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' stopped-initially = "
            << (stoppedInitially ? "true" : "false"));
        return true;
    }

    bool XRotatedFileSinkBintr::Stop()
    {
        LOG_FUNC();

        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_rotationMutex);

        if (!m_isLinked)
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' Stop called while unlinked — no-op");
            return false;
        }
        if (!m_isRecording)
        {
            LOG_INFO("XRotatedFileSinkBintr '" << GetName()
                << "' Stop called while already stopped — no-op");
            return true;
        }

        // B4-PIVOT: close the valve (halts data to container), cancel
        // auto-rotate, finalise the current fragment, tear down the
        // pair. No fakesink swap — the valve is the gate. Parser stays
        // permanently linked to the queue/valve upstream side.

        // Cancel auto-rotation timer if armed — it's for the recording
        // state only. Start() re-arms it.
        if (m_rotationTimerId)
        {
            g_source_remove(m_rotationTimerId);
            m_rotationTimerId = 0;
        }

        // 1) Close the valve.
        _setValveDrop(true);

        // 2) Finalise the current fragment (EOS to container → wait for
        //    arrival at filesink → NULL both). Same _finaliseChildPair
        //    that RotateNow uses.
        DSL_ELEMENT_PTR oldContainer = m_pContainer;
        DSL_ELEMENT_PTR oldFileSink = m_pFileSink;
        std::string closedPath = m_currentFragmentPath;

        _finaliseChildPair(oldContainer, oldFileSink);
        _postFragmentMessage(XROTATED_FILE_FRAGMENT_CLOSED, closedPath);

        // 3) Unlink and drop the old pair. valve's src pad is now
        //    unpeered; with valve.drop=true this is fine.
        m_pValve->UnlinkFromSink();
        RemoveChild(oldContainer);
        RemoveChild(oldFileSink);
        m_pContainer = nullptr;
        m_pFileSink = nullptr;

        m_isRecording = false;

        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' stopped: closed '" << closedPath
            << "' → valve holding upstream until Start()");
        return true;
    }

    bool XRotatedFileSinkBintr::Start()
    {
        LOG_FUNC();

        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_rotationMutex);

        if (!m_isLinked)
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' Start called while unlinked — no-op");
            return false;
        }
        if (m_isRecording)
        {
            LOG_INFO("XRotatedFileSinkBintr '" << GetName()
                << "' Start called while already recording — no-op");
            return true;
        }

        // B4-PIVOT: two paths into Start:
        //  (a) stoppedInitially deploy — m_pContainer / m_pFileSink
        //      still exist (linked from LinkAll, valve closed since
        //      then). Just open the valve; the initial fragment
        //      already has its filesink open.
        //  (b) After Stop() — m_pContainer / m_pFileSink are nullptr.
        //      Need to create a fresh pair, link valve→pair, sync,
        //      open valve.

        if (m_pContainer == nullptr)
        {
            // Post-Stop path.
            uint nextIndex = m_currentIndex + 1;
            std::string nextPath = _renderFragmentPath(nextIndex);

            DSL_ELEMENT_PTR newContainer;
            DSL_ELEMENT_PTR newFileSink;
            if (!_createChildPair(nextPath, newContainer, newFileSink))
            {
                LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                    << "' Start failed to create fresh mp4mux/filesink pair");
                return false;
            }
            if (!m_pValve->LinkToSink(newContainer) or
                !newContainer->LinkToSink(newFileSink))
            {
                LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                    << "' Start failed to link fresh pair to valve");
                return false;
            }
            if (!gst_element_sync_state_with_parent(newContainer->GetGstElement()) or
                !gst_element_sync_state_with_parent(newFileSink->GetGstElement()))
            {
                LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                    << "' sync_state_with_parent returned failure on Start pair");
            }
            m_pContainer = newContainer;
            m_pFileSink = newFileSink;
            m_currentIndex = nextIndex;
            m_currentFragmentPath = nextPath;

            _postFragmentMessage(XROTATED_FILE_FRAGMENT_OPENED,
                m_currentFragmentPath);
        }
        else
        {
            // stoppedInitially deploy — the pair is already linked from
            // LinkAll (fragment 0). Just opening the valve makes it
            // live. Post fragment-opened here (not at LinkAll time) so
            // "start()" is what the JS-side observes as opening the
            // fragment; matches the tactical test's expectation of a
            // fragment-opened event on the first Start after
            // stoppedInitially.
            _postFragmentMessage(XROTATED_FILE_FRAGMENT_OPENED,
                m_currentFragmentPath);
        }

        // Open the valve — buffers flow to the container.
        _setValveDrop(false);
        m_isRecording = true;

        // Re-arm auto-rotate if configured. Cancel any pre-existing
        // timer FIRST — e.g. the one SetMaxSizeTime installed before
        // LinkAll for a stoppedInitially deploy, which the ctor-time
        // Set never got to cancel because Stop hadn't run yet. Without
        // this, we'd leak the pre-existing timer and briefly have two
        // periodic callbacks scheduled.
        if (m_rotationTimerId)
        {
            g_source_remove(m_rotationTimerId);
            m_rotationTimerId = 0;
        }
        if (m_maxSizeTimeNs > 0)
        {
            guint intervalMs =
                (guint)((m_maxSizeTimeNs + 500000) / 1000000);
            if (intervalMs > 0)
            {
                m_rotationTimerId = g_timeout_add(intervalMs,
                    (GSourceFunc)_autoRotateTimerCb, (gpointer)this);
            }
        }

        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' started: opened '" << m_currentFragmentPath << "'");
        return true;
    }

    // -------------------------------------------------------------------
    // B4-PIVOT helper: set valve drop
    // -------------------------------------------------------------------

    void XRotatedFileSinkBintr::_setValveDrop(bool drop)
    {
        if (!m_pValve)
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' _setValveDrop called with null valve");
            return;
        }
        m_pValve->SetAttribute("drop", drop);
        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' valve.drop = " << (drop ? "true" : "false"));
    }

    // B4-PIVOT: _installRotationBlock / _releaseRotationBlock /
    // _installFakeSink / _removeFakeSink all removed. The valve at the
    // top of the container/filesink pair is the single gating mechanism.

    // -------------------------------------------------------------------
    // Private helpers
    // -------------------------------------------------------------------

    std::string XRotatedFileSinkBintr::_renderFragmentPath(uint index)
    {
        // printf into a stack buffer sized for a comfortable path.
        char buf[1024];
        // TODO(xRotatedFileSink): validate the template contains exactly
        //   one printf uint conversion — for now we trust the caller.
        std::snprintf(buf, sizeof(buf), m_filePathTemplate.c_str(), index);
        return std::string(buf);
    }

    bool XRotatedFileSinkBintr::_createChildPair(const std::string& location,
        DSL_ELEMENT_PTR& outContainer, DSL_ELEMENT_PTR& outFileSink)
    {
        // Element names include the fragment index so multiple pairs
        // never collide inside GStreamer's element registry.
        static uint pairIndex = 0;
        uint id = pairIndex++;
        char containerName[256];
        char fileSinkName[256];
        std::snprintf(containerName, sizeof(containerName),
            "%s-qtmux-%u", GetName().c_str(), id);
        std::snprintf(fileSinkName, sizeof(fileSinkName),
            "%s-filesink-%u", GetName().c_str(), id);

        try
        {
            outContainer = DSL_ELEMENT_NEW("qtmux", containerName);
            outFileSink = DSL_ELEMENT_NEW("filesink", fileSinkName);
        }
        catch (...)
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' failed to allocate qtmux + filesink pair");
            return false;
        }

        // Sink property defaults matching FileSinkBintr's contract:
        // async=false to prevent state-change on preroll blocks; sync
        // false to keep the writer independent of upstream clocking.
        outFileSink->SetAttribute("sync", false);
        outFileSink->SetAttribute("async", false);
        outFileSink->SetAttribute("qos", false);
        outFileSink->SetAttribute("enable-last-sample", false);
        outFileSink->SetAttribute("location", location.c_str());

        AddChild(outContainer);
        AddChild(outFileSink);

        return true;
    }

    bool XRotatedFileSinkBintr::_finaliseChildPair(
        DSL_ELEMENT_PTR container, DSL_ELEMENT_PTR filesink)
    {
        // 1) Install an EOS-arrival probe on filesink's sink pad. qtmux
        //    processes an incoming EOS by flushing buffered samples to
        //    mdat, writing moov, and pushing EOS downstream. When the EOS
        //    event surfaces at filesink's sink pad we KNOW both mdat and
        //    moov are on their way to filesink's fd — this is the correct
        //    wait, not the meaningless state-wait we used to do.
        EosArrivalCtx ctx;
        g_mutex_init(&ctx.mutex);
        g_cond_init(&ctx.cond);
        ctx.seen = false;

        GstPad* pFileSinkSinkPad = gst_element_get_static_pad(
            filesink->GetGstElement(), "sink");
        gulong eosProbeId = 0;
        if (pFileSinkSinkPad)
        {
            eosProbeId = gst_pad_add_probe(pFileSinkSinkPad,
                GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                (GstPadProbeCallback)_eosArrivalProbeCb, &ctx, NULL);
        }
        else
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' _finaliseChildPair could not acquire filesink sink pad"
                << " — proceeding without EOS-arrival probe");
        }

        // 2) Send EOS via gst_element_send_event — the DSL-native
        //    pattern (see DslNodetr::SendEos). qtmux processes the
        //    incoming EOS, writes moov, and pushes EOS downstream to
        //    filesink. gst_pad_send_event on an unpeered sink pad was
        //    silently doing nothing.
        if (!gst_element_send_event(container->GetGstElement(),
                                    gst_event_new_eos()))
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' gst_element_send_event(EOS) returned FALSE on container");
        }

        // 3) Bounded wait for EOS to arrive at filesink's sink pad.
        bool eosArrived = false;
        if (pFileSinkSinkPad)
        {
            gint64 endTimeUs = g_get_monotonic_time()
                + (G_TIME_SPAN_SECOND * m_finalizeTimeoutSec);
            g_mutex_lock(&ctx.mutex);
            while (!ctx.seen)
            {
                if (!g_cond_wait_until(&ctx.cond, &ctx.mutex, endTimeUs))
                {
                    break; // timed out
                }
            }
            eosArrived = ctx.seen;
            g_mutex_unlock(&ctx.mutex);

            gst_pad_remove_probe(pFileSinkSinkPad, eosProbeId);
            gst_object_unref(pFileSinkSinkPad);
        }

        if (!eosArrived)
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' EOS did not reach filesink within "
                << m_finalizeTimeoutSec
                << "s — file will likely be truncated (no moov)");
        }

        // 4) NULL filesink FIRST — its state-change to NULL flushes any
        //    pending write buffer and closes the fd on the final,
        //    moov-complete bytes. Then NULL the container.
        gst_element_set_state(filesink->GetGstElement(), GST_STATE_NULL);
        gst_element_set_state(container->GetGstElement(), GST_STATE_NULL);

        g_cond_clear(&ctx.cond);
        g_mutex_clear(&ctx.mutex);
        return eosArrived;
    }

    void XRotatedFileSinkBintr::_postFragmentMessage(const char* structureName,
        const std::string& location)
    {
        GstElement* pGstElement = GetGstElement();
        if (!pGstElement)
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' cannot post fragment message — no GstElement");
            return;
        }
        GstStructure* pStructure = gst_structure_new(structureName,
            "location", G_TYPE_STRING, location.c_str(),
            NULL);
        GstMessage* pMessage = gst_message_new_element(
            GST_OBJECT(pGstElement), pStructure);
        gst_element_post_message(pGstElement, pMessage);
    }

    GstPadProbeReturn XRotatedFileSinkBintr::_rotationBlockProbeCb(
        GstPad* pad, GstPadProbeInfo* info, gpointer userData)
    {
        XRotatedFileSinkBintr* self =
            static_cast<XRotatedFileSinkBintr*>(userData);
        // Flag the pad as blocked — RotateNow's caller sees this and
        // proceeds with the muxer swap.
        self->m_padBlocked = true;
        // Keep the probe installed until RotateNow removes it after the
        // swap is complete.
        return GST_PAD_PROBE_OK;
    }

    gboolean XRotatedFileSinkBintr::_autoRotateTimerCb(gpointer userData)
    {
        XRotatedFileSinkBintr* self =
            static_cast<XRotatedFileSinkBintr*>(userData);
        self->RotateNow();
        return G_SOURCE_CONTINUE;
    }
}
