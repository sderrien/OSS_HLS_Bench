#include <sys/types.h>
#include <stddef.h>

#include "regex_impl.h"
#include "regex2.h"

#define SNAMES
#include "regengine.inc"
#undef SNAMES

#define LNAMES
#include "regengine.inc"
#undef LNAMES

int llvm_regexec(const llvm_regex_t *preg, const char *string, size_t nmatch,
                 llvm_regmatch_t pmatch[], int eflags) {
  struct re_guts *g;

  if (preg == NULL)
    return REG_BADPAT;

  g = preg->re_g;
  if (g == NULL || preg->re_magic != MAGIC1 ||
      g->magic != MAGIC2 || g->pattern == NULL)
    return REG_BADPAT;

  return smatcher(g, string, nmatch, pmatch, eflags);
}
