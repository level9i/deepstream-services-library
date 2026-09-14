// level9i splish-6.3-patched (2026-09-14 valve-splitmuxsink cascade fix)
//
// Unit tests for XRotatedFileSinkBintr — the rolling MP4 sink that
// replaces splitmuxsink in SessionRecordingSink. See
// workspace/docs/proposals/recording-service-phase2/
//   xrotated-file-sink-design.md — design contract.
//
// Tests exercise the class directly (not the FFI C API — see the api/
// suite for that). All tests are private-agnostic: they operate through
// the public method surface only (LinkAll/UnlinkAll/IsLinked/IsRecording
// /Stop/Start/RotateNow/GetMaxSizeTime/SetMaxSizeTime/
// GetCurrentFragmentPath/SetStoppedInitially).

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
#include "DslXRotatedFileSinkBintr.h"

using namespace DSL;

SCENARIO( "A new XRotatedFileSinkBintr is created correctly",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "Attributes for a new XRotatedFileSink" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");
        uint bitrate(0);   // use default
        uint interval(0);

        WHEN( "The XRotatedFileSinkBintr is constructed" )
        {
            DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
                DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                    filePathTemplate.c_str(), bitrate, interval);

            THEN( "The sink defaults are correct" )
            {
                REQUIRE( pSinkBintr->GetName() == sinkName );
                // Default state: recording, unlinked, auto-rotate disabled.
                REQUIRE( pSinkBintr->IsLinked() == false );
                REQUIRE( pSinkBintr->IsRecording() == true );
                REQUIRE( pSinkBintr->GetMaxSizeTime() == 0 );
                REQUIRE( pSinkBintr->GetSyncEnabled() == false );
                REQUIRE( pSinkBintr->GetAsyncEnabled() == false );
                // Current fragment path is derived from template + index 0.
                REQUIRE( pSinkBintr->GetCurrentFragmentPath() != nullptr );
            }
        }
    }
}

SCENARIO( "A new XRotatedFileSinkBintr can LinkAll Child Elementrs",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "A new XRotatedFileSinkBintr in an Unlinked state" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");
        uint bitrate(0);
        uint interval(0);

        DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
            DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                filePathTemplate.c_str(), bitrate, interval);

        REQUIRE( pSinkBintr->IsLinked() == false );
        REQUIRE( pSinkBintr->IsRecording() == true );

        WHEN( "A new XRotatedFileSinkBintr is Linked" )
        {
            REQUIRE( pSinkBintr->LinkAll() == true );

            THEN( "IsLinked is true and IsRecording remains true" )
            {
                REQUIRE( pSinkBintr->IsLinked() == true );
                REQUIRE( pSinkBintr->IsRecording() == true );
            }
        }
    }
}

SCENARIO( "A Linked XRotatedFileSinkBintr can UnlinkAll Child Elementrs",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "A Linked XRotatedFileSinkBintr" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");
        uint bitrate(0);
        uint interval(0);

        DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
            DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                filePathTemplate.c_str(), bitrate, interval);

        REQUIRE( pSinkBintr->LinkAll() == true );
        REQUIRE( pSinkBintr->IsLinked() == true );

        WHEN( "The XRotatedFileSinkBintr is Unlinked" )
        {
            pSinkBintr->UnlinkAll();

            THEN( "IsLinked is false again" )
            {
                REQUIRE( pSinkBintr->IsLinked() == false );
            }
        }
    }
}

SCENARIO( "SetStoppedInitially can be set before LinkAll",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "A new XRotatedFileSinkBintr, not yet linked" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");

        DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
            DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                filePathTemplate.c_str(), 0, 0);

        REQUIRE( pSinkBintr->IsLinked() == false );

        WHEN( "SetStoppedInitially(true) is called before LinkAll" )
        {
            REQUIRE( pSinkBintr->SetStoppedInitially(true) == true );

            THEN( "LinkAll deploys the sink in the stopped state" )
            {
                REQUIRE( pSinkBintr->LinkAll() == true );
                REQUIRE( pSinkBintr->IsLinked() == true );
                REQUIRE( pSinkBintr->IsRecording() == false );
            }
        }
    }
}

SCENARIO( "SetStoppedInitially fails after LinkAll",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "A Linked XRotatedFileSinkBintr" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");

        DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
            DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                filePathTemplate.c_str(), 0, 0);

        REQUIRE( pSinkBintr->LinkAll() == true );
        REQUIRE( pSinkBintr->IsLinked() == true );

        WHEN( "SetStoppedInitially(true) is called after LinkAll" )
        {
            bool result = pSinkBintr->SetStoppedInitially(true);

            THEN( "The call fails (design: must be set before Link)" )
            {
                REQUIRE( result == false );
            }
        }
    }
}

SCENARIO( "Stop transitions a recording XRotatedFileSinkBintr to stopped",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "A Linked, recording XRotatedFileSinkBintr" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");

        DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
            DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                filePathTemplate.c_str(), 0, 0);

        REQUIRE( pSinkBintr->LinkAll() == true );
        REQUIRE( pSinkBintr->IsRecording() == true );

        WHEN( "Stop() is invoked" )
        {
            REQUIRE( pSinkBintr->Stop() == true );

            THEN( "IsRecording flips to false" )
            {
                REQUIRE( pSinkBintr->IsRecording() == false );
            }
        }
    }
}

SCENARIO( "Stop on a stopped XRotatedFileSinkBintr is a no-op",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "A Linked, stopped XRotatedFileSinkBintr" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");

        DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
            DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                filePathTemplate.c_str(), 0, 0);

        REQUIRE( pSinkBintr->SetStoppedInitially(true) == true );
        REQUIRE( pSinkBintr->LinkAll() == true );
        REQUIRE( pSinkBintr->IsRecording() == false );

        WHEN( "Stop() is invoked while already stopped" )
        {
            bool result = pSinkBintr->Stop();

            THEN( "The call succeeds and IsRecording stays false" )
            {
                REQUIRE( result == true );
                REQUIRE( pSinkBintr->IsRecording() == false );
            }
        }
    }
}

SCENARIO( "Start transitions a stopped XRotatedFileSinkBintr to recording",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "A Linked, stopped XRotatedFileSinkBintr" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");

        DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
            DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                filePathTemplate.c_str(), 0, 0);

        REQUIRE( pSinkBintr->SetStoppedInitially(true) == true );
        REQUIRE( pSinkBintr->LinkAll() == true );
        REQUIRE( pSinkBintr->IsRecording() == false );

        WHEN( "Start() is invoked" )
        {
            REQUIRE( pSinkBintr->Start() == true );

            THEN( "IsRecording flips to true" )
            {
                REQUIRE( pSinkBintr->IsRecording() == true );
            }
        }
    }
}

SCENARIO( "Start on a recording XRotatedFileSinkBintr is a no-op",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "A Linked, recording XRotatedFileSinkBintr" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");

        DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
            DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                filePathTemplate.c_str(), 0, 0);

        REQUIRE( pSinkBintr->LinkAll() == true );
        REQUIRE( pSinkBintr->IsRecording() == true );

        WHEN( "Start() is invoked while already recording" )
        {
            bool result = pSinkBintr->Start();

            THEN( "The call succeeds and IsRecording stays true" )
            {
                REQUIRE( result == true );
                REQUIRE( pSinkBintr->IsRecording() == true );
            }
        }
    }
}

SCENARIO( "XRotatedFileSinkBintr survives a full Stop/Start/Stop cycle",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "A Linked, recording XRotatedFileSinkBintr" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");

        DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
            DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                filePathTemplate.c_str(), 0, 0);

        REQUIRE( pSinkBintr->LinkAll() == true );
        REQUIRE( pSinkBintr->IsRecording() == true );

        WHEN( "Stop → Start → Stop is invoked" )
        {
            REQUIRE( pSinkBintr->Stop() == true );
            REQUIRE( pSinkBintr->IsRecording() == false );

            REQUIRE( pSinkBintr->Start() == true );
            REQUIRE( pSinkBintr->IsRecording() == true );

            REQUIRE( pSinkBintr->Stop() == true );

            THEN( "The final state is stopped and the sink is still linked" )
            {
                REQUIRE( pSinkBintr->IsRecording() == false );
                REQUIRE( pSinkBintr->IsLinked() == true );
            }
        }
    }
}

SCENARIO( "MaxSizeTime is a round-trippable property",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "A new XRotatedFileSinkBintr with auto-rotation disabled" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");

        DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
            DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                filePathTemplate.c_str(), 0, 0);

        REQUIRE( pSinkBintr->GetMaxSizeTime() == 0 );

        WHEN( "SetMaxSizeTime is called with a non-zero value" )
        {
            const uint64_t fifteenMinNs = 15ULL * 60ULL * 1000000000ULL;
            REQUIRE( pSinkBintr->SetMaxSizeTime(fifteenMinNs) == true );

            THEN( "GetMaxSizeTime returns the new value" )
            {
                REQUIRE( pSinkBintr->GetMaxSizeTime() == fifteenMinNs );
            }
        }
        WHEN( "SetMaxSizeTime is called with 0 to disable" )
        {
            const uint64_t fiveMinNs = 5ULL * 60ULL * 1000000000ULL;
            REQUIRE( pSinkBintr->SetMaxSizeTime(fiveMinNs) == true );
            REQUIRE( pSinkBintr->GetMaxSizeTime() == fiveMinNs );

            REQUIRE( pSinkBintr->SetMaxSizeTime(0) == true );

            THEN( "GetMaxSizeTime returns 0" )
            {
                REQUIRE( pSinkBintr->GetMaxSizeTime() == 0 );
            }
        }
    }
}

SCENARIO( "GetCurrentFragmentPath returns a valid c-string",
    "[XRotatedFileSinkBintr]" )
{
    GIVEN( "A new XRotatedFileSinkBintr with a template location" )
    {
        std::string sinkName("xrotated-file-sink");
        std::string filePathTemplate("./roll-%02d.mp4");

        DSL_X_ROTATED_FILE_SINK_PTR pSinkBintr =
            DSL_X_ROTATED_FILE_SINK_NEW(sinkName.c_str(),
                filePathTemplate.c_str(), 0, 0);

        WHEN( "GetCurrentFragmentPath is called on a freshly-constructed sink" )
        {
            const char* path = pSinkBintr->GetCurrentFragmentPath();

            THEN( "A non-null path derived from the template is returned" )
            {
                REQUIRE( path != nullptr );
                // The first-fragment path substitutes index 0 into the
                // template — either "./roll-00.mp4" or similar. We assert
                // only that the string is non-empty; the exact rendering
                // is a design decision we don't lock here.
                REQUIRE( std::string(path).length() > 0 );
            }
        }
    }
}
