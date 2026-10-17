//-----------------------------------------------------------------------------
//
// DOOM for the browser - main program.
//
// Same as linuxdoom-1.10/i_main.c. D_DoomMain never returns: the engine
// loop runs as-is and hands control back to the browser through
// Emscripten's asyncify at the points marked [WEB] in the engine.
//
//-----------------------------------------------------------------------------

#include "doomdef.h"

#include "m_argv.h"
#include "d_main.h"

int
main
( int		argc,
  char**	argv )
{
    myargc = argc;
    myargv = argv;

    D_DoomMain ();

    return 0;
}
