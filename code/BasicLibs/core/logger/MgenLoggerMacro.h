#ifndef __MGEN_LOGGER_MACRO_H__
#define __MGEN_LOGGER_MACRO_H__

#include "MgenLogger.h"

// For below STEP_MACRO
extern int current_stage;

/* ----------------------------------------------------------------------------------------------------------------
 | Define MACRO
 +--------------------------------------------------------------------------------------------------------------- */
#define STEP_RESET() { std::cout << std::endl; current_stage = 1; }

#define STEP_START( stage_info ) \
 {\
     MLOG_INFO( "-------------------------------------------------------------------------------------------" );\
     MLOG_INFO( "#%02d. %s", current_stage, stage_info );\
 }

 #define STEP_LINE \
    do { MLOG_INFO("-------------------------------------------------------------------------------------------"); } while(0)

#define STEP_CHECK( callstr ) \
 {\
     bool ret = callstr;\
     if ( ret == false ) {\
         MLOG_ERROR("===> FAILED (#%02d), %s:%d => %s()", current_stage, __FILE__, __LINE__, __func__ );\
         return false;\
     }\
 }

#define STEP_DONE() { MLOG_INFO( "===> DONE (#%02d)", current_stage++ ); }
/* ---------------------------------------------------------------------------------------------------------------- */

#endif