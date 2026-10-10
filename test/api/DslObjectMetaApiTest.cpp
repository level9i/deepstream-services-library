/*
The MIT License

Copyright (c) 2026, Prominence AI, Inc.

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
#include "DslDisplayTypes.h"   // for MAX_DISPLAY_LEN

#include <cstring>

// ---------------------------------------------------------------------------
// Tests for dsl_object_meta_display_text_set — thin FFI utility that mutates
// an NvDsObjectMeta's text_params.display_text slot. These tests exercise
// the function with stack-allocated NvDsObjectMeta instances (zero-initialised)
// so the per-call pointer invariants + memory discipline can be verified
// without a running pipeline.
// ---------------------------------------------------------------------------

SCENARIO( "dsl_object_meta_display_text_set rejects a NULL object_meta",
    "[object-meta-api]" )
{
    GIVEN( "A NULL object_meta pointer" )
    {
        WHEN( "dsl_object_meta_display_text_set is called" )
        {
            THEN( "DSL_RESULT_INVALID_INPUT_PARAM is returned" )
            {
                REQUIRE( dsl_object_meta_display_text_set(NULL, L"anything")
                    == DSL_RESULT_INVALID_INPUT_PARAM );
            }
        }
    }
}

SCENARIO( "dsl_object_meta_display_text_set populates display_text on non-empty input",
    "[object-meta-api]" )
{
    GIVEN( "A zero-initialised NvDsObjectMeta" )
    {
        NvDsObjectMeta objectMeta = {};   // all fields zeroed including text_params

        REQUIRE( objectMeta.text_params.display_text == NULL );

        WHEN( "A non-empty text is set" )
        {
            const wchar_t* wtext = L"person 5";
            const char* expected = "person 5";

            REQUIRE( dsl_object_meta_display_text_set(&objectMeta, wtext)
                == DSL_RESULT_SUCCESS );

            THEN( "display_text points to a g_malloc0'd buffer carrying the text" )
            {
                REQUIRE( objectMeta.text_params.display_text != NULL );
                REQUIRE( std::strcmp(objectMeta.text_params.display_text, expected) == 0 );

                // Clean up (the function's own g_malloc0'd buffer).
                g_free(objectMeta.text_params.display_text);
                objectMeta.text_params.display_text = NULL;
            }
        }
    }
}

SCENARIO( "dsl_object_meta_display_text_set truncates to MAX_DISPLAY_LEN - 1 bytes",
    "[object-meta-api]" )
{
    GIVEN( "A zero-initialised NvDsObjectMeta and a text longer than MAX_DISPLAY_LEN" )
    {
        NvDsObjectMeta objectMeta = {};
        std::wstring longText(MAX_DISPLAY_LEN + 32, L'A');   // 32 bytes over the limit

        WHEN( "The over-length text is set" )
        {
            REQUIRE( dsl_object_meta_display_text_set(&objectMeta, longText.c_str())
                == DSL_RESULT_SUCCESS );

            THEN( "The copied content is bounded to MAX_DISPLAY_LEN - 1 and NUL-terminated" )
            {
                REQUIRE( objectMeta.text_params.display_text != NULL );
                // The buffer is MAX_DISPLAY_LEN bytes; copy length is
                // MAX_DISPLAY_LEN - 1; final byte stays zero (g_malloc0).
                REQUIRE( std::strlen(objectMeta.text_params.display_text)
                    == static_cast<size_t>(MAX_DISPLAY_LEN - 1) );
                REQUIRE( objectMeta.text_params.display_text[MAX_DISPLAY_LEN - 1] == '\0' );

                g_free(objectMeta.text_params.display_text);
                objectMeta.text_params.display_text = NULL;
            }
        }
    }
}

SCENARIO( "dsl_object_meta_display_text_set clears display_text on NULL text",
    "[object-meta-api]" )
{
    GIVEN( "An NvDsObjectMeta with a previously-populated display_text" )
    {
        NvDsObjectMeta objectMeta = {};
        objectMeta.text_params.set_bg_clr = true;

        // Seed display_text as if a prior call had populated it.
        REQUIRE( dsl_object_meta_display_text_set(&objectMeta, L"stale text")
            == DSL_RESULT_SUCCESS );
        REQUIRE( objectMeta.text_params.display_text != NULL );

        WHEN( "The text is cleared with a NULL argument" )
        {
            REQUIRE( dsl_object_meta_display_text_set(&objectMeta, NULL)
                == DSL_RESULT_SUCCESS );

            THEN( "display_text is NULL and set_bg_clr is false" )
            {
                REQUIRE( objectMeta.text_params.display_text == NULL );
                REQUIRE( objectMeta.text_params.set_bg_clr == false );
            }
        }
    }
}

SCENARIO( "dsl_object_meta_display_text_set clears display_text on empty text",
    "[object-meta-api]" )
{
    GIVEN( "An NvDsObjectMeta with a previously-populated display_text" )
    {
        NvDsObjectMeta objectMeta = {};
        objectMeta.text_params.set_bg_clr = true;

        REQUIRE( dsl_object_meta_display_text_set(&objectMeta, L"stale text")
            == DSL_RESULT_SUCCESS );
        REQUIRE( objectMeta.text_params.display_text != NULL );

        WHEN( "The text is cleared with an empty-string argument" )
        {
            REQUIRE( dsl_object_meta_display_text_set(&objectMeta, L"")
                == DSL_RESULT_SUCCESS );

            THEN( "display_text is NULL and set_bg_clr is false" )
            {
                REQUIRE( objectMeta.text_params.display_text == NULL );
                REQUIRE( objectMeta.text_params.set_bg_clr == false );
            }
        }
    }
}

SCENARIO( "dsl_object_meta_display_text_set frees the previous allocation before writing a new one",
    "[object-meta-api]" )
{
    GIVEN( "An NvDsObjectMeta with a previously-populated display_text" )
    {
        NvDsObjectMeta objectMeta = {};

        REQUIRE( dsl_object_meta_display_text_set(&objectMeta, L"first text")
            == DSL_RESULT_SUCCESS );
        REQUIRE( objectMeta.text_params.display_text != NULL );
        REQUIRE( std::strcmp(objectMeta.text_params.display_text, "first text") == 0 );

        WHEN( "A second text is set" )
        {
            // The function must g_free the first allocation internally and
            // replace it with a fresh g_malloc0'd buffer carrying the new text.
            REQUIRE( dsl_object_meta_display_text_set(&objectMeta, L"second text")
                == DSL_RESULT_SUCCESS );

            THEN( "display_text carries the second text (no leak observable at the API boundary)" )
            {
                REQUIRE( objectMeta.text_params.display_text != NULL );
                REQUIRE( std::strcmp(objectMeta.text_params.display_text, "second text") == 0 );

                g_free(objectMeta.text_params.display_text);
                objectMeta.text_params.display_text = NULL;
            }
        }
    }
}
