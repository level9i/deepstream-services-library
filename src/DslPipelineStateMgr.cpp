/*
The MIT License

Copyright (c) 2021, Prominence AI, Inc.


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
#include "DslPipelineStateMgr.h"

namespace DSL
{
    PipelineStateMgr::PipelineStateMgr(GstObject* pGstPipeline)
        : m_pGstPipeline(pGstPipeline)
        , m_pMainContext(NULL)
        , m_pMainLoop(NULL)
        , m_pBusWatch(NULL)
        , m_eosFlag(false)
        , m_errorNotificationTimerId(0)
        , m_busMessageNotificationTimerId(0)
        , m_lastBusMessageType(0)
    {
        LOG_FUNC();

        _initMaps();

        m_pGstBus = gst_pipeline_get_bus(GST_PIPELINE(m_pGstPipeline));

        // Add the bus-watch and callback function to the default main context
        m_busWatchId = gst_bus_add_watch(m_pGstBus, bus_watch, this);
    }

    PipelineStateMgr::~PipelineStateMgr()
    {
        LOG_FUNC();

        // Cancel any pending bus-message notification timer BEFORE the
        // object dies — a g_timeout_add source outlives its `this` pointer
        // by design, so a pending 1ms timer that fires post-destruction
        // dereferences freed memory. Small window in normal operation, but
        // exactly when a shutdown burst of bus messages coincides with
        // pipeline destruction the window opens up. The existing
        // m_errorNotificationTimerId has the same latent race and should
        // be fixed alongside — separate BL, not this patch.
        if (m_busMessageNotificationTimerId != 0)
        {
            g_source_remove(m_busMessageNotificationTimerId);
            m_busMessageNotificationTimerId = 0;
        }

        if (m_pMainLoop)
        {
            DeleteMainLoop();
        }
        gst_bus_remove_watch(m_pGstBus);
        gst_object_unref(m_pGstBus);
    }

    bool PipelineStateMgr::NewMainLoop()
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_busWatchMutex);

        if (m_pMainLoop)
        {
            LOG_ERROR("A main-loop has already been created for Pipeline '"
                << gst_object_get_name(m_pGstPipeline) << "'");
            return false;
        }

        // We need remove the current bus watch added to the default main-context
        gst_bus_remove_watch(m_pGstBus);
        m_busWatchId = 0;
        
        // Create own main-context for the Pipeline first
        m_pMainContext = g_main_context_new();
        if (!m_pMainContext)
        {
            LOG_ERROR("Pipeline '" << gst_object_get_name(m_pGstPipeline) 
                << "' failed to create own main-context");
            return false;
        }

        // Create the main-loop for the Pipeline to run in its own main-context
        m_pMainLoop = g_main_loop_new(m_pMainContext, FALSE);
        if (!m_pMainLoop)
        {
            LOG_ERROR("Pipeline '" << gst_object_get_name(m_pGstPipeline)
                << "' failed to create main-loop");
            return false;
        }
        
        // Create a new bus-watch 
        m_pBusWatch = gst_bus_create_watch(m_pGstBus);

        // Setup our bus-watch callback and then attach the bus-watch to 
        // the Pipeline's own main-context created above.
        g_source_set_callback(m_pBusWatch, (GSourceFunc)bus_watch, this, NULL);
        g_source_attach(m_pBusWatch, m_pMainContext);
        
        return true;
    }
    
    bool PipelineStateMgr::RunMainLoop()
    {
        LOG_FUNC();
        
        if (!m_pMainLoop)
        {
            LOG_ERROR("A Main-Loop has NOT been created for Pipeline '"
                << gst_object_get_name(m_pGstPipeline) << "'");
            return false;
        }
        if (g_main_loop_is_running(m_pMainLoop))
        {
            LOG_ERROR("A Main-Loop is already running for Pipeline '"
                << gst_object_get_name(m_pGstPipeline) << "'");
            return false;
        }
        // Acquire context and set it as the thread-default context for the current thread.
        g_main_context_push_thread_default(m_pMainContext);
        
        // call will block until QuitMainLoop is called from another thread.
        g_main_loop_run(m_pMainLoop);
        
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_mainLoopMutex);
        
        // Pop context off the thread-default context stack and signal client
        g_main_context_pop_thread_default(m_pMainContext);
        
        g_cond_signal(&m_mainLoopCond);
        
        return true;
    }
    
    
    bool PipelineStateMgr::QuitMainLoop()
    {
        LOG_FUNC();

        if (!m_pMainLoop)
        {
            LOG_ERROR("A Main-Loop has NOT been created for Pipeline '"
                << gst_object_get_name(m_pGstPipeline) << "'");
            return false;
        }
        if (!g_main_loop_is_running(m_pMainLoop))
        {
            LOG_ERROR("Main-loop for Pipeline '"
                << gst_object_get_name(m_pGstPipeline) << "' is not running");
            return false;
        }
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_mainLoopMutex);
        g_main_loop_quit(m_pMainLoop);
        g_cond_wait(&m_mainLoopCond, &m_mainLoopMutex);
        
        return true;
    }
    
    bool PipelineStateMgr::DeleteMainLoop()
    {
        LOG_FUNC();
        
        if (!m_pMainLoop)
        {
            LOG_ERROR("A Main-Loop has NOT been created for Pipeline '"
                << gst_object_get_name(m_pGstPipeline) << "'");
            return false;
        }
        // destroy the bus-watch - which unattaches the bus-watch from the main-context
        g_source_destroy(m_pBusWatch);
        
        g_main_loop_unref(m_pMainLoop);
        g_main_context_unref(m_pMainContext);
        m_pBusWatch = NULL;
        m_pMainLoop = NULL;
        m_pMainContext = NULL;

        // re-install the watch function for the message bus with the default 
        // main-context - setting it back to its default state.
        m_busWatchId = gst_bus_add_watch(m_pGstBus, bus_watch, this);
        
        return true;
    }

    bool PipelineStateMgr::AddStateChangeListener(dsl_state_change_listener_cb listener, void* clientData)
    {
        LOG_FUNC();
        
        if (m_stateChangeListeners.find(listener) != m_stateChangeListeners.end())
        {   
            LOG_ERROR("Pipeline listener is not unique");
            return false;
        }
        m_stateChangeListeners[listener] = clientData;
        
        return true;
    }

    bool PipelineStateMgr::RemoveStateChangeListener(dsl_state_change_listener_cb listener)
    {
        LOG_FUNC();
        
        if (m_stateChangeListeners.find(listener) == m_stateChangeListeners.end())
        {   
            LOG_ERROR("Pipeline listener was not found");
            return false;
        }
        m_stateChangeListeners.erase(listener);
        
        return true;
    }

    bool PipelineStateMgr::AddEosListener(dsl_eos_listener_cb listener, void* clientData)
    {
        LOG_FUNC();
        
        if (m_eosListeners.find(listener) != m_eosListeners.end())
        {   
            LOG_ERROR("Pipeline listener is not unique");
            return false;
        }
        m_eosListeners[listener] = clientData;
        
        return true;
    }

    bool PipelineStateMgr::IsEosListener(dsl_eos_listener_cb listener)
    {
        LOG_FUNC();
        
        return (m_eosListeners.find(listener) != m_eosListeners.end());
    }

    bool PipelineStateMgr::RemoveEosListener(dsl_eos_listener_cb listener)
    {
        LOG_FUNC();
        
        if (m_eosListeners.find(listener) == m_eosListeners.end())
        {   
            LOG_ERROR("Pipeline listener was not found");
            return false;
        }
        m_eosListeners.erase(listener);
        
        return true;
    }

    bool PipelineStateMgr::AddErrorMessageHandler(dsl_error_message_handler_cb handler, void* clientData)
    {
        LOG_FUNC();
        
        if (m_errorMessageHandlers.find(handler) != m_errorMessageHandlers.end())
        {   
            LOG_ERROR("Pipeline handler is not unique");
            return false;
        }
        m_errorMessageHandlers[handler] = clientData;
        
        return true;
    }

    bool PipelineStateMgr::RemoveErrorMessageHandler(dsl_error_message_handler_cb handler)
    {
        LOG_FUNC();
        
        if (m_errorMessageHandlers.find(handler) == m_errorMessageHandlers.end())
        {   
            LOG_ERROR("Pipeline handler was not found");
            return false;
        }
        m_errorMessageHandlers.erase(handler);
        
        return true;
    }
    
    bool PipelineStateMgr::AddBusMessageHandler(dsl_bus_message_handler_cb handler, void* clientData)
    {
        LOG_FUNC();

        if (m_busMessageHandlers.find(handler) != m_busMessageHandlers.end())
        {
            LOG_ERROR("Pipeline bus-message handler is not unique");
            return false;
        }
        m_busMessageHandlers[handler] = clientData;

        return true;
    }

    bool PipelineStateMgr::RemoveBusMessageHandler(dsl_bus_message_handler_cb handler)
    {
        LOG_FUNC();

        if (m_busMessageHandlers.find(handler) == m_busMessageHandlers.end())
        {
            LOG_ERROR("Pipeline bus-message handler was not found");
            return false;
        }
        m_busMessageHandlers.erase(handler);

        return true;
    }

    bool PipelineStateMgr::HandleBusWatchMessage(GstMessage* pMessage)
    {
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_busWatchMutex);

        GstClockTime clockTime;
        GstStreamStatusType statusType;
        GstElement* pElement(NULL);
        GstFormat format(GST_FORMAT_UNDEFINED);
        guint64 processed(0);
        guint64 dropped(0);
        GError* error(NULL);
        gchar* debugInfo(NULL);
        gint percent(0);
        gchar* propertyName(NULL);
        GstProgressType progressType(GST_PROGRESS_TYPE_ERROR);
        gchar* code;
        gchar* text;

        const gchar* name = gst_message_type_get_name(GST_MESSAGE_TYPE(pMessage));

        switch (GST_MESSAGE_TYPE(pMessage))
        {
        case GST_MESSAGE_ASYNC_DONE:
            gst_message_parse_async_done(pMessage, &clockTime);
            LOG_INFO("Message type : " << name);
            LOG_INFO("   source    : " << GST_OBJECT_NAME(pMessage->src));
            LOG_INFO("   time      : " << clockTime);
            break;
            
        case GST_MESSAGE_STREAM_STATUS:
            gst_message_parse_stream_status(pMessage, &statusType, &pElement);
            LOG_INFO("Message type : " << name);
            LOG_INFO("   source    : " << GST_OBJECT_NAME(pMessage->src));
            LOG_INFO("   type      : " << statusType);
            LOG_INFO("   element   : " << GST_ELEMENT_NAME(pElement));
            break;
            
        case GST_MESSAGE_QOS:
            gst_message_parse_qos_stats(pMessage, &format, &processed, &dropped);
            LOG_INFO("Message type : " << name);
            LOG_INFO("   source    : " << GST_OBJECT_NAME(pMessage->src));
            LOG_INFO("   format    : " << gst_format_get_name(format));
            LOG_INFO("   processed : " << processed);
            LOG_INFO("   dropped   : " << dropped);
            break;
            
        case GST_MESSAGE_BUFFERING:
            gst_message_parse_buffering(pMessage, &percent);
            LOG_INFO("Message type : " << name);
            LOG_INFO("   source    : " << GST_OBJECT_NAME(pMessage->src));
            LOG_INFO("   percent   : " << percent);
            break;
            
        case GST_MESSAGE_LATENCY:
            LOG_INFO("Message type : " << name);
            break;
            
        case GST_MESSAGE_PROGRESS:
            gst_message_parse_progress(pMessage,
                &progressType, &code, &text);
            LOG_INFO("Message type : " << name);
            LOG_INFO("   source    : " << GST_OBJECT_NAME(pMessage->src));
            LOG_INFO("   type      : " << progressType);
            LOG_INFO("   code      : " << code);
            LOG_INFO("   text      : " << text);
            break;
            
        case GST_MESSAGE_INFO:
            gst_message_parse_info(pMessage, &error, &debugInfo);
            LOG_INFO("Message type : " << name);
            LOG_INFO("   info      : " << error->message);
            if(debugInfo)
                LOG_INFO("   debug     : " << debugInfo);
            g_error_free(error);
            g_free(debugInfo);
            break;

        case GST_MESSAGE_WARNING:
            gst_message_parse_warning(pMessage, &error, &debugInfo);
            LOG_INFO("Message type : " << name);
            LOG_INFO("   warning   : " << error->message);
            if(debugInfo)
                LOG_INFO("   debug     : " << debugInfo);
            g_error_free(error);
            g_free(debugInfo);
            break;
            
        case GST_MESSAGE_EOS:
            HandleEosMessage(pMessage);
            break;
        case GST_MESSAGE_ERROR:
            HandleErrorMessage(pMessage);            
            break;
        case GST_MESSAGE_STATE_CHANGED:
            HandleStateChanged(pMessage);
            break;
        case GST_MESSAGE_APPLICATION:
            HandleApplicationMessage(pMessage);
            break;

        case GST_MESSAGE_ELEMENT:
        case GST_MESSAGE_DURATION_CHANGED:
        case GST_MESSAGE_NEW_CLOCK:
        case GST_MESSAGE_TAG:
            break;
        default:
            LOG_INFO("Unhandled message type:: " << name);
        }

        // Generic bus-message dispatch — fire registered handlers for the
        // four categories that have no dedicated typed handler. Handler
        // dispatch is deferred to the main loop via g_timeout_add so
        // client callbacks never run on the bus thread.
        //
        // Included categories (why each is here):
        //   GST_MESSAGE_ELEMENT     — element-authored (splitmuxsink-fragment-*,
        //                             nvv4l2-encoder-QoS-report, etc). The main
        //                             motivator for this handler existing.
        //   GST_MESSAGE_APPLICATION — application-injected via gst_element_post_message
        //                             (custom pipeline instrumentation).
        //   GST_MESSAGE_WARNING     — non-fatal condition reports; useful
        //                             observability without needing a dedicated
        //                             warning-handler API surface.
        //   GST_MESSAGE_INFO        — informational messages (rarely emitted,
        //                             included for symmetry with WARNING).
        //
        // Excluded categories (why each is out):
        //   GST_MESSAGE_ERROR         — dedicated dsl_pipeline_error_message_handler_add
        //   GST_MESSAGE_EOS           — dedicated dsl_pipeline_eos_listener_add
        //   GST_MESSAGE_STATE_CHANGED — dedicated dsl_pipeline_state_change_listener_add
        //   GST_MESSAGE_BUFFERING     — dedicated dsl_pipeline_buffering_message_handler
        //   GST_MESSAGE_QOS           — chatty; per-buffer QoS is not what a
        //                               generic handler wants to firehose.
        //   GST_MESSAGE_STREAM_STATUS — internal streaming-thread lifecycle;
        //                               noise at the pipeline-observation level.
        //   GST_MESSAGE_ASYNC_DONE    — state-change internal completion;
        //                               already exposed via state-change listener.
        //   GST_MESSAGE_LATENCY       — recalculation trigger, not an event
        //                               to observe.
        //   GST_MESSAGE_PROGRESS      — long-operation heartbeats, currently unused.
        //   GST_MESSAGE_DURATION_CHANGED / NEW_CLOCK / TAG — noise for our use.
        if (m_busMessageHandlers.size())
        {
            GstMessageType type = GST_MESSAGE_TYPE(pMessage);
            if (type == GST_MESSAGE_ELEMENT ||
                type == GST_MESSAGE_APPLICATION ||
                type == GST_MESSAGE_WARNING ||
                type == GST_MESSAGE_INFO)
            {
                LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_lastBusMessageMutex);

                m_lastBusMessageType = static_cast<uint32_t>(type);

                const gchar* srcName = pMessage->src ?
                    GST_OBJECT_NAME(pMessage->src) : NULL;
                if (srcName)
                {
                    std::string cstrSrc(srcName);
                    m_lastBusMessageSourceElementName =
                        std::wstring(cstrSrc.begin(), cstrSrc.end());
                }
                else
                {
                    m_lastBusMessageSourceElementName.clear();
                }

                const GstStructure* structure = gst_message_get_structure(pMessage);
                if (structure)
                {
                    const gchar* structName = gst_structure_get_name(structure);
                    if (structName)
                    {
                        std::string cstrStructName(structName);
                        m_lastBusMessageStructureName =
                            std::wstring(cstrStructName.begin(), cstrStructName.end());
                    }
                    else
                    {
                        m_lastBusMessageStructureName.clear();
                    }
                    gchar* structStr = gst_structure_to_string(structure);
                    if (structStr)
                    {
                        // ⚠️ CAVEAT: byte-to-wchar widening (not a decode) —
                        // element/structure names are ASCII in practice, but
                        // structure_serialised carries arbitrary element-authored
                        // content that CAN include non-ASCII (file paths on
                        // non-ASCII filesystems, tag payloads with unicode).
                        // Byte values > 0x7F silently corrupt on this widening.
                        // Callers who need full unicode fidelity should decode
                        // from a separate UTF-8 surface (not offered here).
                        std::string cstrStructStr(structStr);
                        m_lastBusMessageStructureSerialised =
                            std::wstring(cstrStructStr.begin(), cstrStructStr.end());
                        g_free(structStr);
                    }
                    else
                    {
                        m_lastBusMessageStructureSerialised.clear();
                    }
                }
                else
                {
                    m_lastBusMessageStructureName.clear();
                    m_lastBusMessageStructureSerialised.clear();
                }

                m_busMessageNotificationTimerId = g_timeout_add(1,
                    BusMessageHandlersNotificationHandler, this);
            }
        }

        return true;
    }

    bool PipelineStateMgr::HandleStateChanged(GstMessage* pMessage)
    {
        if (GST_ELEMENT(GST_MESSAGE_SRC(pMessage)) != GST_ELEMENT(m_pGstPipeline))
        {
            return false;
        }

        GstState oldstate, newstate;
        gst_message_parse_state_changed(pMessage, &oldstate, &newstate, NULL);

        LOG_INFO(m_mapPipelineStates[oldstate] << " => " << m_mapPipelineStates[newstate]);

        // iterate through the map of state-change-listeners calling each
        for(auto const& imap: m_stateChangeListeners)
        {
            try
            {
                imap.first((uint)oldstate, (uint)newstate, imap.second);
            }
            catch(...)
            {
                LOG_ERROR("Exception calling Client State-Change-Listener");
            }
        }
        return true;
    }
    
    void PipelineStateMgr::HandleEosMessage(GstMessage* pMessage)
    {
        LOG_INFO("EOS message recieved for Pipeline '" 
            << gst_object_get_name(m_pGstPipeline) << "'");
        
        // If the EOS event was sent from HandleStop
        if (m_eosFlag)
        {
            return;
        }
        
        // Action EOS so set the flag
        m_eosFlag = true;
        
        // iterate through the map of EOS-listeners calling each
        for(auto const& imap: m_eosListeners)
        {
            try
            {
                imap.first(imap.second);
            }
            catch(...)
            {
                LOG_ERROR("Exception calling Client EOS-Lister");
            }
        }
    }
    
    void PipelineStateMgr::HandleApplicationMessage(GstMessage* pMessage)
    {
        LOG_FUNC();
        
        const GstStructure* msgPayload = gst_message_get_structure(pMessage);

        // only one application message at this time. 
        if(gst_structure_has_name(msgPayload, "stop-pipline"))
        {
            HandleStop();
        }
        else
        {
            LOG_ERROR("Unknown Application message received by Pipeline '"
                << gst_object_get_name(m_pGstPipeline) << "'");
        }
    }
    
    void PipelineStateMgr::HandleErrorMessage(GstMessage* pMessage)
    {
        LOG_FUNC();
        
        GError* error = NULL;
        gchar* debugInfo = NULL;
        gst_message_parse_error(pMessage, &error, &debugInfo);

        LOG_ERROR("Error message '" << error->message << "' received from '" 
            << GST_OBJECT_NAME(pMessage->src) << "'");
            
        if (debugInfo)
        {
            LOG_DEBUG("Debug info: " << debugInfo);
        }

        // persist the last error information
        std::string cstrSource(GST_OBJECT_NAME(pMessage->src));
        std::string cstrMessage(error->message);

        std::wstring wstrSource(cstrSource.begin(), cstrSource.end());
        std::wstring wstrMessage(cstrMessage.begin(), cstrMessage.end());
        
        // Setting the last error message will invoke a timer thread to notify all client handlers.
        SetLastErrorMessage(wstrSource, wstrMessage);
        
        g_error_free(error);
        g_free(debugInfo);
    }    

    void PipelineStateMgr::GetLastErrorMessage(std::wstring& source, std::wstring& message)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_lastErrorMutex);
        
        source = m_lastErrorSource;
        message = m_lastErrorMessage;
    }

    void PipelineStateMgr::SetLastErrorMessage(std::wstring& source, std::wstring& message)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_lastErrorMutex);

        m_lastErrorSource = source;
        m_lastErrorMessage = message;
        
        if (m_errorMessageHandlers.size())
        {
            m_errorNotificationTimerId = g_timeout_add(1, ErrorMessageHandlersNotificationHandler, this);
        }
    }
    
    int PipelineStateMgr::NotifyErrorMessageHandlers()
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_lastErrorMutex);

        // iterate through the map of state-change-listeners calling each
        for(auto const& imap: m_errorMessageHandlers)
        {
            try
            {
                imap.first(m_lastErrorSource.c_str(), m_lastErrorMessage.c_str(), imap.second);
            }
            catch(...)
            {
                LOG_ERROR("PipelineStateMgr threw exception calling Client Error-Message-Handler");
            }
        }
        // clear the timer id and return false to self remove
        m_errorNotificationTimerId = 0;
        return false;
    }

    int PipelineStateMgr::NotifyBusMessageHandlers()
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_lastBusMessageMutex);

        // Build a stack-allocated info snapshot from member state. The
        // wchar_t* pointers are owned by our member wstrings and remain
        // valid for the duration of this method (mutex held throughout).
        dsl_bus_message_info info;
        info.message_type = m_lastBusMessageType;
        info.source_element_name = m_lastBusMessageSourceElementName.empty()
            ? NULL : m_lastBusMessageSourceElementName.c_str();
        info.structure_name = m_lastBusMessageStructureName.empty()
            ? NULL : m_lastBusMessageStructureName.c_str();
        info.structure_serialised = m_lastBusMessageStructureSerialised.empty()
            ? NULL : m_lastBusMessageStructureSerialised.c_str();

        for(auto const& imap: m_busMessageHandlers)
        {
            try
            {
                imap.first(&info, imap.second);
            }
            catch(...)
            {
                LOG_ERROR("PipelineStateMgr threw exception calling Client Bus-Message-Handler");
            }
        }
        m_busMessageNotificationTimerId = 0;
        return false;
    }

    void PipelineStateMgr::_initMaps()
    {
        m_mapPipelineStates[GST_STATE_READY] = "GST_STATE_READY";
        m_mapPipelineStates[GST_STATE_PLAYING] = "GST_STATE_PLAYING";
        m_mapPipelineStates[GST_STATE_PAUSED] = "GST_STATE_PAUSED";
        m_mapPipelineStates[GST_STATE_NULL] = "GST_STATE_NULL";
    }
    
    static gboolean bus_watch(GstBus* bus, GstMessage* pMessage, gpointer pPipeline)
    {
        return static_cast<PipelineStateMgr*>(pPipeline)->HandleBusWatchMessage(pMessage);
    }    
    
    static int ErrorMessageHandlersNotificationHandler(gpointer pPipeline)
    {
        return static_cast<PipelineStateMgr*>(pPipeline)->
            NotifyErrorMessageHandlers();
    }

    static int BusMessageHandlersNotificationHandler(gpointer pPipeline)
    {
        return static_cast<PipelineStateMgr*>(pPipeline)->
            NotifyBusMessageHandlers();
    }

} // DSL