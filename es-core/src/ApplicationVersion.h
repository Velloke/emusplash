//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  ApplicationVersion.h
//

#ifndef ES_CORE_APPLICATION_VERSION_H
#define ES_CORE_APPLICATION_VERSION_H

// These numbers and strings need to be manually updated for a new version.
// Do this version number update as the very last commit for the new release version.
// clang-format off
#define PROGRAM_VERSION_MAJOR        1
#define PROGRAM_VERSION_MINOR        0
#define PROGRAM_VERSION_MAINTENANCE  2
// The release of the code this build is based on, the theme downloader compares it with the
// release its theme list asks for (and the settings file keeps it).
#define PROGRAM_RELEASE_NUMBER       53
// clang-format on
#define PROGRAM_VERSION_STRING "1.0.2"

#define PROGRAM_BUILT_STRING __DATE__ " - " __TIME__

#define RESOURCE_VERSION_STRING "1,0,2\0"
#define RESOURCE_VERSION PROGRAM_VERSION_MAJOR, PROGRAM_VERSION_MINOR, PROGRAM_VERSION_MAINTENANCE

#endif // ES_CORE_APPLICATION_VERSION_H
