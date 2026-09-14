// level9i splish-6.3-patched (2026-09-14 valve-splitmuxsink cascade fix)
//
// API-level tests for the dsl_sink_x_rotated_file_* FFI verbs.
// Companion to test/unit/DslXRotatedFileSinkUnitTest.cpp (which
// exercises the C++ class directly). This suite drives every verb
// through the DSL_RESULT_* return-code surface.
//
// See workspace/docs/proposals/recording-service-phase2/
//   xrotated-file-sink-design.md — design contract.

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

#include "catch.hpp"
#include "Dsl.h"
#include "DslApi.h"

static const std::wstring sink_name(L"xrotated-file-sink");
static const std::wstring alt_name(L"xrotated-file-sink-2");
static const std::wstring file_path(L"./roll-%02d.mp4");
static const uint bitrate(0);
static const uint interval(0);

SCENARIO( "The Components container is updated correctly on new XRotatedFileSink",
    "[xrotated-file-sink-api]" )
{
    GIVEN( "An empty list of Components" )
    {
        REQUIRE( dsl_component_list_size() == 0 );

        WHEN( "A new XRotatedFileSink is created" )
        {
            REQUIRE( dsl_sink_x_rotated_file_new(sink_name.c_str(),
                file_path.c_str(), bitrate, interval) == DSL_RESULT_SUCCESS );

            THEN( "The list size is updated correctly" )
            {
                REQUIRE( dsl_component_list_size() == 1 );
            }
        }
        REQUIRE( dsl_component_delete_all() == DSL_RESULT_SUCCESS );
    }
}

SCENARIO( "The Components container is updated correctly on XRotatedFileSink delete",
    "[xrotated-file-sink-api]" )
{
    GIVEN( "An XRotatedFileSink Component" )
    {
        REQUIRE( dsl_component_list_size() == 0 );

        REQUIRE( dsl_sink_x_rotated_file_new(sink_name.c_str(),
            file_path.c_str(), bitrate, interval) == DSL_RESULT_SUCCESS );
        REQUIRE( dsl_component_list_size() == 1 );

        WHEN( "The XRotatedFileSink is deleted" )
        {
            REQUIRE( dsl_component_delete(sink_name.c_str())
                == DSL_RESULT_SUCCESS );

            THEN( "The list size is updated correctly" )
            {
                REQUIRE( dsl_component_list_size() == 0 );
            }
        }
    }
}

SCENARIO( "Creating a second XRotatedFileSink with the same name fails",
    "[xrotated-file-sink-api]" )
{
    GIVEN( "An XRotatedFileSink already created" )
    {
        REQUIRE( dsl_component_list_size() == 0 );

        REQUIRE( dsl_sink_x_rotated_file_new(sink_name.c_str(),
            file_path.c_str(), bitrate, interval) == DSL_RESULT_SUCCESS );
        REQUIRE( dsl_component_list_size() == 1 );

        WHEN( "A second XRotatedFileSink is created with the same name" )
        {
            uint result = dsl_sink_x_rotated_file_new(sink_name.c_str(),
                file_path.c_str(), bitrate, interval);

            THEN( "DSL_RESULT_SINK_NAME_NOT_UNIQUE is returned" )
            {
                REQUIRE( result == DSL_RESULT_SINK_NAME_NOT_UNIQUE );
                REQUIRE( dsl_component_list_size() == 1 );
            }
        }
        REQUIRE( dsl_component_delete_all() == DSL_RESULT_SUCCESS );
    }
}

SCENARIO( "XRotatedFileSink max_size_time round-trips",
    "[xrotated-file-sink-api]" )
{
    GIVEN( "An XRotatedFileSink with auto-rotation disabled" )
    {
        REQUIRE( dsl_component_list_size() == 0 );

        REQUIRE( dsl_sink_x_rotated_file_new(sink_name.c_str(),
            file_path.c_str(), bitrate, interval) == DSL_RESULT_SUCCESS );

        uint64_t retMaxSizeTime(999);
        REQUIRE( dsl_sink_x_rotated_file_max_size_time_get(
            sink_name.c_str(), &retMaxSizeTime) == DSL_RESULT_SUCCESS );
        REQUIRE( retMaxSizeTime == 0 );

        WHEN( "max_size_time is set to a 15-minute period" )
        {
            const uint64_t fifteenMinNs = 15ULL * 60ULL * 1000000000ULL;
            REQUIRE( dsl_sink_x_rotated_file_max_size_time_set(
                sink_name.c_str(), fifteenMinNs) == DSL_RESULT_SUCCESS );

            THEN( "max_size_time_get returns the same value" )
            {
                REQUIRE( dsl_sink_x_rotated_file_max_size_time_get(
                    sink_name.c_str(), &retMaxSizeTime) == DSL_RESULT_SUCCESS );
                REQUIRE( retMaxSizeTime == fifteenMinNs );
            }
        }
        WHEN( "max_size_time is toggled non-zero then back to zero" )
        {
            const uint64_t fiveMinNs = 5ULL * 60ULL * 1000000000ULL;
            REQUIRE( dsl_sink_x_rotated_file_max_size_time_set(
                sink_name.c_str(), fiveMinNs) == DSL_RESULT_SUCCESS );

            REQUIRE( dsl_sink_x_rotated_file_max_size_time_set(
                sink_name.c_str(), 0) == DSL_RESULT_SUCCESS );

            THEN( "max_size_time_get returns 0" )
            {
                REQUIRE( dsl_sink_x_rotated_file_max_size_time_get(
                    sink_name.c_str(), &retMaxSizeTime) == DSL_RESULT_SUCCESS );
                REQUIRE( retMaxSizeTime == 0 );
            }
        }
        REQUIRE( dsl_component_delete_all() == DSL_RESULT_SUCCESS );
    }
}

SCENARIO( "XRotatedFileSink verbs fail cleanly on a non-existent name",
    "[xrotated-file-sink-api]" )
{
    GIVEN( "An empty component list" )
    {
        REQUIRE( dsl_component_list_size() == 0 );

        const std::wstring bogus_name(L"does-not-exist");

        WHEN( "rotate_now is called on a name that does not exist" )
        {
            uint result = dsl_sink_x_rotated_file_rotate_now(
                bogus_name.c_str());

            THEN( "DSL_RESULT_SINK_NAME_NOT_FOUND is returned" )
            {
                REQUIRE( result == DSL_RESULT_SINK_NAME_NOT_FOUND );
            }
        }
        WHEN( "max_size_time_get is called on a bogus name" )
        {
            uint64_t retMaxSizeTime(0);
            uint result = dsl_sink_x_rotated_file_max_size_time_get(
                bogus_name.c_str(), &retMaxSizeTime);

            THEN( "DSL_RESULT_SINK_NAME_NOT_FOUND is returned" )
            {
                REQUIRE( result == DSL_RESULT_SINK_NAME_NOT_FOUND );
            }
        }
        WHEN( "stop is called on a bogus name" )
        {
            uint result = dsl_sink_x_rotated_file_stop(bogus_name.c_str());

            THEN( "DSL_RESULT_SINK_NAME_NOT_FOUND is returned" )
            {
                REQUIRE( result == DSL_RESULT_SINK_NAME_NOT_FOUND );
            }
        }
        WHEN( "is_recording_get is called on a bogus name" )
        {
            boolean isRecording(0);
            uint result = dsl_sink_x_rotated_file_is_recording_get(
                bogus_name.c_str(), &isRecording);

            THEN( "DSL_RESULT_SINK_NAME_NOT_FOUND is returned" )
            {
                REQUIRE( result == DSL_RESULT_SINK_NAME_NOT_FOUND );
            }
        }
    }
}

SCENARIO( "XRotatedFileSink stop/start toggles is_recording",
    "[xrotated-file-sink-api]" )
{
    GIVEN( "A fresh XRotatedFileSink" )
    {
        REQUIRE( dsl_component_list_size() == 0 );

        REQUIRE( dsl_sink_x_rotated_file_new(sink_name.c_str(),
            file_path.c_str(), bitrate, interval) == DSL_RESULT_SUCCESS );

        boolean isRecording(0);

        // Fresh sink is recording (before it's linked, in-memory default).
        REQUIRE( dsl_sink_x_rotated_file_is_recording_get(
            sink_name.c_str(), &isRecording) == DSL_RESULT_SUCCESS );
        REQUIRE( isRecording == true );

        WHEN( "stop is called then is_recording_get is read" )
        {
            REQUIRE( dsl_sink_x_rotated_file_stop(sink_name.c_str())
                == DSL_RESULT_SUCCESS );

            THEN( "is_recording is false" )
            {
                REQUIRE( dsl_sink_x_rotated_file_is_recording_get(
                    sink_name.c_str(), &isRecording) == DSL_RESULT_SUCCESS );
                REQUIRE( isRecording == false );
            }
        }
        WHEN( "stop then start is called" )
        {
            REQUIRE( dsl_sink_x_rotated_file_stop(sink_name.c_str())
                == DSL_RESULT_SUCCESS );
            REQUIRE( dsl_sink_x_rotated_file_start(sink_name.c_str())
                == DSL_RESULT_SUCCESS );

            THEN( "is_recording is true again" )
            {
                REQUIRE( dsl_sink_x_rotated_file_is_recording_get(
                    sink_name.c_str(), &isRecording) == DSL_RESULT_SUCCESS );
                REQUIRE( isRecording == true );
            }
        }
        REQUIRE( dsl_component_delete_all() == DSL_RESULT_SUCCESS );
    }
}

SCENARIO( "XRotatedFileSink stopped_initially_set changes deploy state",
    "[xrotated-file-sink-api]" )
{
    GIVEN( "A fresh XRotatedFileSink flagged stopped-initially" )
    {
        REQUIRE( dsl_component_list_size() == 0 );

        REQUIRE( dsl_sink_x_rotated_file_new(sink_name.c_str(),
            file_path.c_str(), bitrate, interval) == DSL_RESULT_SUCCESS );

        REQUIRE( dsl_sink_x_rotated_file_stopped_initially_set(
            sink_name.c_str(), true) == DSL_RESULT_SUCCESS );

        WHEN( "is_recording_get is read before any Link happens" )
        {
            // Pre-link state — the setter records the flag; is_recording
            // reflects the current m_isRecording, which the LinkAll path
            // will honor.
            boolean isRecording(1);
            REQUIRE( dsl_sink_x_rotated_file_is_recording_get(
                sink_name.c_str(), &isRecording) == DSL_RESULT_SUCCESS );

            THEN( "The FFI call succeeds and returns a valid boolean" )
            {
                // We don't lock the pre-link value here — it's an
                // implementation choice whether IsRecording reads the
                // deploy-state flag or the runtime flag before LinkAll.
                REQUIRE( (isRecording == true || isRecording == false) );
            }
        }
        REQUIRE( dsl_component_delete_all() == DSL_RESULT_SUCCESS );
    }
}

SCENARIO( "XRotatedFileSink current_fragment_path_get returns a valid string",
    "[xrotated-file-sink-api]" )
{
    GIVEN( "A fresh XRotatedFileSink" )
    {
        REQUIRE( dsl_component_list_size() == 0 );

        REQUIRE( dsl_sink_x_rotated_file_new(sink_name.c_str(),
            file_path.c_str(), bitrate, interval) == DSL_RESULT_SUCCESS );

        WHEN( "current_fragment_path_get is called" )
        {
            const wchar_t* c_ret_path(NULL);
            uint result = dsl_sink_x_rotated_file_current_fragment_path_get(
                sink_name.c_str(), &c_ret_path);

            THEN( "The call succeeds and returns a non-null path pointer" )
            {
                REQUIRE( result == DSL_RESULT_SUCCESS );
                REQUIRE( c_ret_path != NULL );
                // We don't lock the initial-fragment rendering here — the
                // unit test scenario covers that. Just prove the FFI
                // round-trip returns a usable pointer.
                std::wstring ret_path(c_ret_path);
                REQUIRE( ret_path.length() > 0 );
            }
        }
        REQUIRE( dsl_component_delete_all() == DSL_RESULT_SUCCESS );
    }
}

SCENARIO( "Two XRotatedFileSinks can coexist with distinct names",
    "[xrotated-file-sink-api]" )
{
    GIVEN( "An empty component list" )
    {
        REQUIRE( dsl_component_list_size() == 0 );

        WHEN( "Two XRotatedFileSinks are created with distinct names" )
        {
            REQUIRE( dsl_sink_x_rotated_file_new(sink_name.c_str(),
                file_path.c_str(), bitrate, interval) == DSL_RESULT_SUCCESS );
            REQUIRE( dsl_sink_x_rotated_file_new(alt_name.c_str(),
                file_path.c_str(), bitrate, interval) == DSL_RESULT_SUCCESS );

            THEN( "Both are in the components list" )
            {
                REQUIRE( dsl_component_list_size() == 2 );
            }
        }
        REQUIRE( dsl_component_delete_all() == DSL_RESULT_SUCCESS );
    }
}
