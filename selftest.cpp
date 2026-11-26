// quill · selftest.cpp — placeholder while the core is brought up; replaced by the real self-test.
#include "app.h"

namespace quill {

Answer verb_selftest() {
    Answer a;
    a.verb = "selftest";
    a.status = "error";
    Issue i;
    i.code = "E_INTERNAL";
    i.message = "the self-test is not written yet";
    a.issues.push_back(i);
    a.text = "quill selftest: not written yet\n";
    return a;
}

}  // namespace quill
