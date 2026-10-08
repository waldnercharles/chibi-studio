#ifndef CHIBI_TEST_ASSET_FIXTURE_H
#define CHIBI_TEST_ASSET_FIXTURE_H

#include "support/asset_text.h"
#include "support/asset_value.h"

#include <assert.h>

/* Each fixture value has independent ownership, like captured runtime values. */
static inline av_value *test_read_asset_value(const char *text)
{
    char error[256];
    av_doc *doc = av_doc_read(text, error, sizeof error);
    assert(doc);
    av_value *value = av_value_copy(av_doc_root(doc));
    av_doc_free(doc);
    return value;
}

#endif
