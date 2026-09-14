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
        , m_pFakeSink(nullptr)
    {
        LOG_FUNC();

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

        // Encoder chain is always linked. The tail differs depending on
        // whether we deploy in the recording or stopped state.
        if (!m_pQueue->LinkToSink(m_pTransform) or
            !m_pTransform->LinkToSink(m_pCapsFilter) or
            !m_pCapsFilter->LinkToSink(m_pEncoder) or
            !m_pEncoder->LinkToSink(m_pParser))
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' failed to link encoder chain");
            return false;
        }

        if (m_stoppedInitially)
        {
            // Discard the pre-created initial container/filesink pair —
            // deploy in the stopped state with a fakesink tail.
            if (m_pContainer)
            {
                RemoveChild(m_pContainer);
                m_pContainer = nullptr;
            }
            if (m_pFileSink)
            {
                RemoveChild(m_pFileSink);
                m_pFileSink = nullptr;
            }
            if (!_installFakeSink())
            {
                LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                    << "' failed to install initial fakesink for stopped deploy");
                return false;
            }
            m_isRecording = false;
            m_isLinked = true;
            LOG_INFO("XRotatedFileSinkBintr '" << GetName()
                << "' linked in STOPPED state; awaiting Start()");
            return true;
        }

        // Recording deploy — link the initial container/filesink pair.
        if (!m_pParser->LinkToSink(m_pContainer) or
            !m_pContainer->LinkToSink(m_pFileSink))
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' failed to link initial container/filesink");
            return false;
        }

        m_isLinked = true;

        // First fragment is now open — post the opened message.
        _postFragmentMessage(XROTATED_FILE_FRAGMENT_OPENED,
            m_currentFragmentPath);
        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' linked; initial fragment opened at '"
            << m_currentFragmentPath << "'");

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

        // Unlink the tail element based on current state.
        if (m_isRecording && m_pContainer)
        {
            m_pContainer->UnlinkFromSink();
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

        // Prepare the next fragment path FIRST so we can pass it into
        // the fresh child pair without racing on m_currentIndex.
        uint nextIndex = m_currentIndex + 1;
        std::string nextPath = _renderFragmentPath(nextIndex);

        // Install a downstream-block pad probe on the encoder's src pad.
        // This idles the flow before we tear down the muxer, so we do
        // not drop mid-buffer or corrupt the container.
        GstPad* pProbePad = gst_element_get_static_pad(
            m_pEncoder->GetGstElement(), "src");
        if (!pProbePad)
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' RotateNow could not acquire encoder src pad");
            return false;
        }

        m_padBlocked = false;
        m_rotationProbeId = gst_pad_add_probe(pProbePad,
            GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
            (GstPadProbeCallback)_rotationBlockProbeCb, this, NULL);

        // Wait bounded for the probe callback to fire (i.e. for a buffer
        // to reach the pad). If nothing is flowing we accept the risk
        // and continue — we then can't guarantee a keyframe boundary,
        // but any buffered data in filesink will still finalise cleanly
        // via the EOS below.
        gint64 endTime = g_get_monotonic_time()
            + (G_TIME_SPAN_SECOND * m_finalizeTimeoutSec);
        {
            DslMutex localMutex;
            LOCK_MUTEX_FOR_CURRENT_SCOPE(&localMutex);
            while (!m_padBlocked
                && g_get_monotonic_time() < endTime)
            {
                // Poll — probe callback flips m_padBlocked from its own thread.
                g_usleep(1000);
            }
        }

        if (!m_padBlocked)
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' RotateNow — no buffer arrived within "
                << m_finalizeTimeoutSec
                << "s; proceeding regardless");
        }

        // Snapshot the current pair before we swap them out.
        DSL_ELEMENT_PTR oldContainer = m_pContainer;
        DSL_ELEMENT_PTR oldFileSink = m_pFileSink;
        std::string closedPath = m_currentFragmentPath;

        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' rotation: finalising '" << closedPath
            << "' BEFORE any unlink (parser + container + filesink all"
            << " still linked; buffers held upstream at encoder-src probe)");

        // FINALIZE FIRST, UNLINK AFTER. Do NOT unlink parser →
        // oldContainer before finalize — for muxers with request sink
        // pads (qtmux), the unlink releases the request pad, after
        // which the EOS we inject has no pad to reach. The correct
        // order is:
        //  a) Send EOS while parser → oldContainer → oldFileSink is
        //     fully linked; block probe upstream keeps new data out.
        //  b) Wait for EOS to reach oldFileSink's sink pad.
        //  c) NULL oldFileSink then oldContainer.
        //  d) THEN unlink parser (the link is dead by now anyway).
        _finaliseChildPair(oldContainer, oldFileSink);
        _postFragmentMessage(XROTATED_FILE_FRAGMENT_CLOSED, closedPath);

        // Now safe to unlink and drop the old pair.
        m_pParser->UnlinkFromSink();
        RemoveChild(oldContainer);
        RemoveChild(oldFileSink);

        // Create the fresh pair.
        DSL_ELEMENT_PTR newContainer;
        DSL_ELEMENT_PTR newFileSink;
        if (!_createChildPair(nextPath, newContainer, newFileSink))
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' RotateNow failed to create new mp4mux/filesink pair");
            // Release the block probe to unstick the pipeline.
            gst_pad_remove_probe(pProbePad, m_rotationProbeId);
            gst_object_unref(pProbePad);
            m_rotationProbeId = 0;
            return false;
        }

        // Link parser → new container → new filesink.
        if (!m_pParser->LinkToSink(newContainer) or
            !newContainer->LinkToSink(newFileSink))
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' RotateNow failed to link new pair");
            gst_pad_remove_probe(pProbePad, m_rotationProbeId);
            gst_object_unref(pProbePad);
            m_rotationProbeId = 0;
            return false;
        }

        // Sync state on the new elements so they enter PLAYING before
        // the probe is released and buffers flow.
        if (!gst_element_sync_state_with_parent(newContainer->GetGstElement()) or
            !gst_element_sync_state_with_parent(newFileSink->GetGstElement()))
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' sync_state_with_parent returned failure on new pair");
        }

        // Update state.
        m_pContainer = newContainer;
        m_pFileSink = newFileSink;
        m_currentIndex = nextIndex;
        m_currentFragmentPath = nextPath;

        // Release the pad probe — buffers flow into the fresh mp4mux.
        gst_pad_remove_probe(pProbePad, m_rotationProbeId);
        gst_object_unref(pProbePad);
        m_rotationProbeId = 0;

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

        // Block upstream flow at the encoder src pad (same probe point as
        // RotateNow).
        GstPad* pProbePad = nullptr;
        _installRotationBlock(pProbePad);
        if (!pProbePad)
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' Stop could not acquire encoder src pad");
            return false;
        }

        // Cancel auto-rotation timer if armed — it's for the recording
        // state only. Start() re-arms it.
        if (m_rotationTimerId)
        {
            g_source_remove(m_rotationTimerId);
            m_rotationTimerId = 0;
        }

        // Snapshot the current pair so we can dispose after unlink.
        DSL_ELEMENT_PTR oldContainer = m_pContainer;
        DSL_ELEMENT_PTR oldFileSink = m_pFileSink;
        std::string closedPath = m_currentFragmentPath;

        // Unlink parser → oldContainer only. Leave oldContainer →
        // oldFileSink intact so EOS can propagate through qtmux to
        // filesink, delivering the moov trailer to disk. See the
        // matching change in RotateNow for why pre-unlinking that link
        // is the wrong thing.
        m_pParser->UnlinkFromSink();

        // Finalise the closed fragment (EOS → bounded wait → NULL).
        _finaliseChildPair(oldContainer, oldFileSink);
        _postFragmentMessage(XROTATED_FILE_FRAGMENT_CLOSED, closedPath);

        // Drop the old children from this Bintr.
        RemoveChild(oldContainer);
        RemoveChild(oldFileSink);
        m_pContainer = nullptr;
        m_pFileSink = nullptr;

        // Attach the fakesink tail.
        if (!_installFakeSink())
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' Stop failed to install fakesink");
            _releaseRotationBlock(pProbePad);
            return false;
        }

        m_isRecording = false;

        _releaseRotationBlock(pProbePad);

        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' stopped: closed '" << closedPath
            << "' → fakesink absorbing until Start()");
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

        GstPad* pProbePad = nullptr;
        _installRotationBlock(pProbePad);
        if (!pProbePad)
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' Start could not acquire encoder src pad");
            return false;
        }

        // Detach fakesink.
        _removeFakeSink();

        // Compute next fragment path from the current index.
        uint nextIndex = m_currentIndex + 1;
        std::string nextPath = _renderFragmentPath(nextIndex);

        // Create the fresh container/filesink pair.
        DSL_ELEMENT_PTR newContainer;
        DSL_ELEMENT_PTR newFileSink;
        if (!_createChildPair(nextPath, newContainer, newFileSink))
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' Start failed to create fresh mp4mux/filesink pair");
            _releaseRotationBlock(pProbePad);
            return false;
        }

        // Link parser → newContainer → newFileSink.
        if (!m_pParser->LinkToSink(newContainer) or
            !newContainer->LinkToSink(newFileSink))
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' Start failed to link new pair");
            _releaseRotationBlock(pProbePad);
            return false;
        }

        // Sync state on the new elements so they enter PLAYING before we
        // release the probe and buffers flow.
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
        m_isRecording = true;

        _postFragmentMessage(XROTATED_FILE_FRAGMENT_OPENED,
            m_currentFragmentPath);

        // Re-arm the auto-rotation timer if it was armed by
        // SetMaxSizeTime.
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

        _releaseRotationBlock(pProbePad);

        LOG_INFO("XRotatedFileSinkBintr '" << GetName()
            << "' started: opened '" << m_currentFragmentPath << "'");
        return true;
    }

    // -------------------------------------------------------------------
    // Stop/Start helpers
    // -------------------------------------------------------------------

    bool XRotatedFileSinkBintr::_installRotationBlock(GstPad*& outProbePad)
    {
        outProbePad = gst_element_get_static_pad(
            m_pEncoder->GetGstElement(), "src");
        if (!outProbePad)
        {
            return false;
        }

        m_padBlocked = false;
        m_rotationProbeId = gst_pad_add_probe(outProbePad,
            GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
            (GstPadProbeCallback)_rotationBlockProbeCb, this, NULL);

        gint64 endTime = g_get_monotonic_time()
            + (G_TIME_SPAN_SECOND * m_finalizeTimeoutSec);
        DslMutex localMutex;
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&localMutex);
        while (!m_padBlocked
            && g_get_monotonic_time() < endTime)
        {
            g_usleep(1000);
        }
        if (!m_padBlocked)
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' block probe did not fire within "
                << m_finalizeTimeoutSec << "s; proceeding");
            return false;
        }
        return true;
    }

    void XRotatedFileSinkBintr::_releaseRotationBlock(GstPad* pProbePad)
    {
        if (pProbePad && m_rotationProbeId)
        {
            gst_pad_remove_probe(pProbePad, m_rotationProbeId);
            m_rotationProbeId = 0;
        }
        if (pProbePad)
        {
            gst_object_unref(pProbePad);
        }
    }

    bool XRotatedFileSinkBintr::_installFakeSink()
    {
        if (m_pFakeSink)
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' _installFakeSink called while a fakesink is already"
                << " installed — reusing existing");
            return true;
        }

        char fakeSinkName[256];
        std::snprintf(fakeSinkName, sizeof(fakeSinkName),
            "%s-fakesink", GetName().c_str());

        try
        {
            m_pFakeSink = DSL_ELEMENT_NEW("fakesink", fakeSinkName);
        }
        catch (...)
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' failed to allocate fakesink");
            m_pFakeSink = nullptr;
            return false;
        }

        m_pFakeSink->SetAttribute("sync", false);
        m_pFakeSink->SetAttribute("async", false);
        m_pFakeSink->SetAttribute("qos", false);
        m_pFakeSink->SetAttribute("enable-last-sample", false);

        AddChild(m_pFakeSink);

        // Link parser → fakesink with EXPLICIT h264 avc caps. Without
        // the filter, h264parse's src pad negotiates with fakesink
        // (which accepts anything) and commits to
        // stream-format=byte-stream. When Start() later relinks parser
        // to qtmux (which demands stream-format=avc), gst_element_link
        // sees the caps mismatch and refuses.
        //
        // Bypass DSL's Nodetr::LinkToSink for this transient linkage:
        // parser's Nodetr link-state stays "unlinked" so the future
        // Start() can call DSL::LinkToSink(newContainer) without
        // tripping the "already linked to Sink" pre-check.
        GstCaps* caps = gst_caps_new_simple("video/x-h264",
            "stream-format", G_TYPE_STRING, "avc",
            "alignment", G_TYPE_STRING, "au",
            NULL);
        gboolean linked = gst_element_link_pads_filtered(
            m_pParser->GetGstElement(), "src",
            m_pFakeSink->GetGstElement(), "sink",
            caps);
        gst_caps_unref(caps);
        if (!linked)
        {
            LOG_ERROR("XRotatedFileSinkBintr '" << GetName()
                << "' failed to link parser → fakesink (filtered avc)");
            RemoveChild(m_pFakeSink);
            m_pFakeSink = nullptr;
            return false;
        }

        if (!gst_element_sync_state_with_parent(m_pFakeSink->GetGstElement()))
        {
            LOG_WARN("XRotatedFileSinkBintr '" << GetName()
                << "' sync_state_with_parent returned failure on fakesink");
        }

        return true;
    }

    void XRotatedFileSinkBintr::_removeFakeSink()
    {
        if (!m_pFakeSink)
        {
            return;
        }
        // NULL the fakesink FIRST (mirrors _finaliseChildPair
        // ordering) so we unlink from a NULL peer.
        gst_element_set_state(m_pFakeSink->GetGstElement(), GST_STATE_NULL);

        // RemoveChild (gst_bin_remove) auto-unlinks the fakesink's pads
        // as part of removing it from the bin — this is the same
        // mechanism the rest of the DSL codebase relies on for element
        // teardown. Explicit gst_element_unlink beforehand appears to
        // leave stale peer state on parser's src pad that then blocks
        // the subsequent LinkToSink(newContainer) in Start().
        RemoveChild(m_pFakeSink);
        m_pFakeSink = nullptr;
    }

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
