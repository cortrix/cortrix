#include "cortrix/doc_summary/doc_summary_types.h"

namespace cortrix::doc_summary {

const char* ToString(DocSummaryStatus status) {
    switch (status) {
        case DocSummaryStatus::kPending:   return "pending";
        case DocSummaryStatus::kGenerated: return "generated";
        case DocSummaryStatus::kFailed:    return "failed";
    }
    return "pending";
}

}  // namespace cortrix::doc_summary
