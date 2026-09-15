// level9i splish-6.3-patched (2026-09-14 valve-splitmuxsink cascade fix)
//
// XRotatedFileSinkBintr — a rolling-fragment MP4 sink under our own
// control. Composes `qtmux` + `filesink` internally and rotates on
// either an auto timer (`m_maxSizeTimeNs`) or an explicit
// `RotateNow()` call. Sends EOS to its inner muxer at every rotation
// boundary AND at pipeline-stop, so mp4mux writes its `moov` trailer
// cleanly and filesink closes each file with a valid tail. This is the
// property `splitmuxsink` lacks when its upstream is a `valve` that
// silently drops the EOS that would otherwise trigger finalize.
//
// See workspace/docs/proposals/recording-service-phase2/
//   xrotated-file-sink-design.md — design contract for this class.

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

#ifndef _DSL_X_ROTATED_FILE_SINK_BINTR_H
#define _DSL_X_ROTATED_FILE_SINK_BINTR_H

#include "Dsl.h"
#include "DslApi.h"
#include "DslBintr.h"
#include "DslElementr.h"
#include "DslSinkBintr.h"

namespace DSL
{
    #define DSL_X_ROTATED_FILE_SINK_PTR std::shared_ptr<XRotatedFileSinkBintr>
    #define DSL_X_ROTATED_FILE_SINK_NEW(name, filepath, bitrate, interval) \
        std::shared_ptr<XRotatedFileSinkBintr>( \
        new XRotatedFileSinkBintr(name, filepath, bitrate, interval))

    /**
     * @class XRotatedFileSinkBintr
     * @brief Rolling MP4 sink — mp4mux (qtmux) + filesink, with EOS-first
     * rotation and EOS-first shutdown. Replaces splitmuxsink for
     * schedule-driven session recording where zero-gap rotation is not
     * required but reliable teardown is.
     *
     * Chain (identical shell to FileSinkBintr, only the container +
     * filesink pair are per-fragment):
     *   m_pQueue → m_pTransform → m_pCapsFilter → m_pEncoder → m_pParser
     *              → m_pContainer → m_pFileSink
     */
    class XRotatedFileSinkBintr : public EncodeSinkBintr
    {
    public:

        /**
         * @brief Ctor. Creates the inner qtmux + filesink pair and
         * initialises the first fragment's location from filePathTemplate.
         * @param[in] name unique component name.
         * @param[in] filePathTemplate printf-style location template, e.g.
         *   "/data/roll-%02d.mp4". Fragment index substitutes into the
         *   template on every rotation.
         * @param[in] bitrate encoder bitrate in bits/sec (0 = default).
         * @param[in] interval encoder iframe interval.
         */
        XRotatedFileSinkBintr(const char* name, const char* filePathTemplate,
            uint bitrate, uint interval);

        ~XRotatedFileSinkBintr();

        /**
         * @brief Links all child elements (queue → transform → caps →
         * encoder → parser → container → filesink). Adds a downstream
         * pad probe on the encoder's src pad — the probe is used to
         * BLOCK data flow during rotation; it's a no-op in the steady
         * state.
         */
        bool LinkAll();

        /**
         * @brief Unlinks all child elements.
         */
        void UnlinkAll();

        /**
         * @brief Gets the current auto-rotation period in nanoseconds.
         * @return 0 if auto-rotation is disabled; otherwise the current
         * period in ns.
         */
        uint64_t GetMaxSizeTime();

        /**
         * @brief Sets the auto-rotation period in nanoseconds. Set to 0
         * to disable auto-rotation. When enabled, a GLib timeout fires
         * every N ns and invokes RotateNow().
         * @param[in] maxSizeTimeNs new period in ns; 0 disables.
         * @return true on success.
         */
        bool SetMaxSizeTime(uint64_t maxSizeTimeNs);

        /**
         * @brief Rotate now. Blocks the upstream flow, EOSes the current
         * mp4mux, waits bounded for finalize, swaps in a fresh
         * mp4mux+filesink pair for the next fragment, syncs state, and
         * releases the block. Rotation gap is typically 50-200 ms on
         * Xavier NX.
         * @return true if the rotation completed cleanly. False if the
         * bounded wait timed out (the caller may retry or accept
         * possible short-file on the previous fragment).
         */
        bool RotateNow();

        /**
         * @brief Returns the path of the current fragment on disk.
         * @return current fragment path as C string. Owned by this bin
         * — do not free.
         */
        const char* GetCurrentFragmentPath();

        /**
         * @brief State-change override. On PLAYING→PAUSED (or the
         * equivalent teardown transition) sends an internal EOS to
         * mp4mux so the current file finalises before the parent
         * transition walks the child bin to NULL. This is the crucial
         * divergence from splitmuxsink, which relies on an upstream
         * EOS (never delivered when a valve is dropping buffers).
         */
        bool SetState(GstState state, uint timeoutNs);

        /**
         * @brief Whether the sink is currently producing file fragments
         * (true) or silently discarding encoded frames to an internal
         * fakesink (false).
         */
        bool IsRecording();

        /**
         * @brief Stop file production. Finalises the current fragment
         * cleanly (EOS to muxer + bounded wait), then swaps the
         * container+filesink pair for an internal fakesink. The encoder
         * chain (queue → transform → capsfilter → encoder → parser)
         * keeps running and consuming buffers upstream — this is
         * deliberate. To gate encoder cycles as well, the caller should
         * combine this with an upstream valve. No-op if already stopped.
         * @return true on transition (or already-stopped), false on error.
         */
        bool Stop();

        /**
         * @brief Start file production. Creates a fresh muxer+filesink
         * pair targeting the next fragment path, links it into the
         * chain, and re-arms the auto-rotation timer if
         * `m_maxSizeTimeNs > 0`. No-op if already recording.
         * @return true on transition (or already-recording), false on error.
         */
        bool Start();

        /**
         * @brief Set the initial recording state — must be called BEFORE
         * `LinkAll()`. When true, the sink deploys in the stopped state:
         * no initial mp4mux/filesink pair is linked; a fakesink absorbs
         * encoded frames until `Start()` is called. Default is false
         * (recording from deploy).
         * @param[in] stoppedInitially true = deploy stopped, false = deploy recording.
         * @return true on success, false if called after LinkAll().
         */
        bool SetStoppedInitially(bool stoppedInitially);

    private:

        /**
         * @brief printf-style template for fragment paths.
         */
        std::string m_filePathTemplate;

        /**
         * @brief current fragment index — increments each rotation.
         */
        uint m_currentIndex;

        /**
         * @brief current fragment path (derived from template +
         * m_currentIndex).
         */
        std::string m_currentFragmentPath;

        /**
         * @brief auto-rotation period in ns. 0 = disabled.
         */
        uint64_t m_maxSizeTimeNs;

        /**
         * @brief GLib timeout id for the auto-rotation timer, 0 when
         * inactive.
         */
        guint m_rotationTimerId;

        /**
         * @brief bounded wait for mp4mux finalize during rotation and
         * teardown. Defaults to 5s.
         */
        uint m_finalizeTimeoutSec;

        /**
         * @brief current inner container (qtmux). Recreated on rotate.
         */
        DSL_ELEMENT_PTR m_pContainer;

        /**
         * @brief current inner filesink. Recreated on rotate.
         */
        DSL_ELEMENT_PTR m_pFileSink;

        /**
         * @brief mutex serialising rotation calls.
         */
        DslMutex m_rotationMutex;

        /**
         * @brief condvar to signal completion of the block-probe callback
         * during rotation.
         */
        DslCond m_rotationCond;

        /**
         * @brief current probe id installed on the encoder's src pad
         * during a rotation. 0 when no rotation is in flight.
         */
        gulong m_rotationProbeId;

        /**
         * @brief flag set inside the block-probe callback when the pad
         * has been idled — read by RotateNow to know the swap can
         * proceed.
         */
        bool m_padBlocked;

        /**
         * @brief current recording state. True = valve open, buffers
         * flow to m_pContainer/m_pFileSink; false = valve closed,
         * buffers dropped upstream. Starts true unless
         * SetStoppedInitially(true) was called before LinkAll.
         */
        bool m_isRecording;

        /**
         * @brief deploy-state override — determines whether the initial
         * container/filesink pair is linked at LinkAll time. When true,
         * the valve is closed and the container/filesink pair is NOT
         * linked; the first Start() call creates and links the pair.
         * Ignored after LinkAll has run.
         */
        bool m_stoppedInitially;

        /**
         * @brief B4-PIVOT (2026-09-15): the valve that gates buffer flow
         * from parser to container. Permanently linked between parser
         * and the container/filesink pair. valve.drop=true halts data
         * flow to the container without disturbing parser's src pad.
         *
         * Replaced m_pFakeSink. Rationale: swapping parser's peer
         * (parser→qtmux ⇌ parser→fakesink) left parser's src pad in a
         * state that broke subsequent gst_element_link(parser, qtmux)
         * from returning success on Start-from-stopped. Keeping parser
         * always-linked to a stable valve, with the container swap
         * happening on the OTHER side of the valve, sidesteps the
         * problem entirely.
         */
        DSL_ELEMENT_PTR m_pValve;

        /**
         * @brief B4-PIVOT (2026-09-15): leaky-downstream queue between
         * parser and valve. Absorbs the brief buffer accumulation
         * during rotation/stop/start transitions without back-pressuring
         * the encoder chain. Leaky-downstream means older buffers are
         * dropped when the queue fills — matches the "brief data loss
         * during rotation" tolerance stated in the design.
         */
        DSL_ELEMENT_PTR m_pPostParserQueue;

        // ---------------------------------------------------------
        // Internal helpers (B4-PIVOT retained)
        // ---------------------------------------------------------

        /**
         * @brief Sets the valve's drop property (true = drop buffers,
         * false = pass through). Wraps the SetAttribute + logging.
         * @param[in] drop new drop state.
         */
        void _setValveDrop(bool drop);

        // ---------------------------------------------------------
        // Internal helpers
        // ---------------------------------------------------------

        /**
         * @brief Derive a fragment path from the template + index.
         */
        std::string _renderFragmentPath(uint index);

        /**
         * @brief Create a fresh qtmux + filesink pair for the given
         * location. Adds them to this Bintr as children but does not
         * link them.
         */
        bool _createChildPair(const std::string& location,
            DSL_ELEMENT_PTR& outContainer, DSL_ELEMENT_PTR& outFileSink);

        /**
         * @brief Send EOS to the given container's sink pad and wait
         * (bounded) for filesink to reach READY (i.e. finalised).
         * Regardless of the wait outcome, transitions both to NULL
         * before returning.
         */
        bool _finaliseChildPair(DSL_ELEMENT_PTR container,
            DSL_ELEMENT_PTR filesink);

        /**
         * @brief Post an `xrotatedfile-fragment-opened` or
         * `xrotatedfile-fragment-closed` element message on the pipeline
         * bus carrying the fragment `location`.
         */
        void _postFragmentMessage(const char* structureName,
            const std::string& location);

        /**
         * @brief Pad-probe callback for the rotation block. Signals the
         * rotation condvar so RotateNow() can proceed with the swap.
         */
        static GstPadProbeReturn _rotationBlockProbeCb(GstPad* pad,
            GstPadProbeInfo* info, gpointer userData);

        /**
         * @brief GLib timeout callback for auto-rotation. Invokes
         * RotateNow(). Returns G_SOURCE_CONTINUE so the timer keeps
         * firing until SetMaxSizeTime(0) removes it.
         */
        static gboolean _autoRotateTimerCb(gpointer userData);
    };
}

#endif // _DSL_X_ROTATED_FILE_SINK_BINTR_H
