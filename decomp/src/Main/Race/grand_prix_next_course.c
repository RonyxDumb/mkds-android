#include "Main/Race/grand_prix_next_course.h"
#include "Main/Kart/kart_scale_surface_tables.h"
#include "Main/Race/race_setup_block.h"
#include "touch_panel_sampling.h"
#include "Main/Race/race_setup_input.h"

void SetNextGrandPrixCourse(void) {
    gRaceSetup->mNextHeader.mCourseNumber = gRaceSetup->mCourseNumber + 1;
    if (gRaceSetup->mNextHeader.mCourseNumber <= COURSES_PER_CUP) {
#ifdef PLATFORM_PORT
        u32 GetCupCourseId(s32 cupIndex, s32 courseIndex); // the port's extra cup (oam.c)
        gRaceSetup->mNextHeader.mCourseId =
            GetCupCourseId(gRaceSetup->mNextHeader.mCupIndex, gRaceSetup->mNextHeader.mCourseNumber - 1);
#else
        gRaceSetup->mNextHeader.mCourseId =
            gCupCourseIdTable[gRaceSetup->mNextHeader.mCupIndex][gRaceSetup->mNextHeader.mCourseNumber - 1];
#endif
    }
    gRaceSetup->mNextHeader.mRaceKind = RACE_KIND_START_GP;
}
