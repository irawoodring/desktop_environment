/* version.h — which commit pane was built from.
 *
 * Built in a git checkout, the Makefile passes it in. Built from a tarball that
 * `git archive` made (as LakerLinux's build does), git has already written it in
 * below: .gitattributes marks this file "export-subst". Anything else: "unknown". */
#pragma once

#ifndef PANE_COMMIT
#define PANE_COMMIT "$Format:%h$"
#endif

static inline const char *pane_commit(void) {
    return PANE_COMMIT[0] == '$' ? "unknown" : PANE_COMMIT;
}
