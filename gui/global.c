/**
 * @file
 * Global functions
 *
 * @authors
 * Copyright (C) 2022 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page gui_global Global functions
 *
 * Global functions
 */

#include "config.h"
#include <stddef.h>
#include "mutt/lib.h"
#include "core/lib.h"
#include "gui/lib.h"
#include "global.h"
#include "lib.h"
#include "index/lib.h"
#include "muttlib.h"
#include "opcodes.h"

/**
 * op_version - Show the NeoMutt version number - Implements ::global_function_t - @ingroup global_function_api
 */
static int op_version(int op)
{
  mutt_message(mutt_make_version());
  return IR_SUCCESS;
}

/**
 * op_what_key - display the keycode for a key press - Implements ::global_function_t - @ingroup global_function_api
 */
static int op_what_key(int op)
{
  mutt_what_key();
  return IR_SUCCESS;
}

/**
 * GlobalFunctions - All the NeoMutt functions that the Global supports
 */
struct GlobalFunction GlobalFunctions[] = {
  // clang-format off
  { OP_VERSION,               op_version },
  { OP_WHAT_KEY,              op_what_key },
  { 0, NULL },
  // clang-format on
};

/**
 * global_function_dispatcher - Perform a Global function
 * @param win Window
 * @param op  Operation to perform, e.g. OP_VERSION
 * @retval num #IndexRetval, e.g. #IR_SUCCESS
 */
int global_function_dispatcher(struct MuttWindow *win, int op)
{
  int rc = IR_UNKNOWN;
  for (size_t i = 0; GlobalFunctions[i].op != OP_NULL; i++)
  {
    const struct GlobalFunction *fn = &GlobalFunctions[i];
    if (fn->op == op)
    {
      rc = fn->function(op);
      break;
    }
  }

  if (rc == IR_UNKNOWN) // Not our function
    return rc;

  const char *result = mutt_map_get_name(rc, RetvalNames);
  mutt_debug(LL_DEBUG1, "Handled %s (%d) -> %s\n", OpStrings[op][0], op, NONULL(result));

  return IR_SUCCESS; // Whatever the outcome, we handled it
}

/**
 * traverse_tree - Traverse a tree of Windows to find function to handle an operation
 * @param win    Window to start at
 * @param ignore Child Window to ignore
 * @param op     Operation to perform, e.g. OP_VERSION
 * @retval num #IndexRetval, e.g. #IR_SUCCESS
 *
 * Descend through a tree of Windows.  If a Window has a function dispatcher, run it.
 * If it can handle the operation, then finish.
 *
 * Non-visible windows will be ignored.
 */
static int traverse_tree(struct MuttWindow *win, struct MuttWindow *ignore, int op)
{
  if (!win || !win->state.visible)
    return IR_UNKNOWN;

  int rc;
  if (win->function)
  {
    rc = win->function(win, op);
    if (rc != IR_UNKNOWN)
      return rc;
  }

  struct MuttWindow *np = NULL;
  TAILQ_FOREACH(np, &win->children, entries)
  {
    if (np == ignore)
      continue;

    rc = traverse_tree(np, NULL, op);
    if (rc != IR_UNKNOWN)
      return rc;
  }

  return IR_UNKNOWN;
}

/**
 * window_dispatch_function - Search for a handler for an operation
 * @param win Window to start at
 * @param op  Operation to perform, e.g. OP_VERSION
 * @retval num #IndexRetval, e.g. #IR_SUCCESS
 *
 * Search through a tree of Windows looking for one with a function dispatcher
 * that can handle op.
 *
 * Start at the given Window, then search it's children.
 * If that fails climb the tree looking wider for matches.
 */
int window_dispatch_function(struct MuttWindow *win, int op)
{
  if (!mutt_window_is_visible(win))
    return IR_UNKNOWN;

  struct MuttWindow *ignore = NULL;

  int rc;
  for (; win; ignore = win, win = win->parent)
  {
    rc = traverse_tree(win, ignore, op);
    if (rc != IR_UNKNOWN)
      return rc;
  }

  return IR_UNKNOWN;
}
