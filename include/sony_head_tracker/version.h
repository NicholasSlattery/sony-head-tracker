/* version.h
 * SINGLE SOURCE OF TRUTH for the product version. Bump this one file to release.
 * It is consumed by:
 *   - version.hpp   (C++: sony::kVersion)
 *   - app.rc        (Win32 VERSIONINFO: FILEVERSION / PRODUCTVERSION / strings)
 * CI fails a tagged build whose executable version does not match the tag, so
 * these can never silently drift. (The manifest's assembly-identity version is
 * intentionally static and is not a product version -- see app.manifest.)
 */
#ifndef SONY_HEAD_TRACKER_VERSION_H
#define SONY_HEAD_TRACKER_VERSION_H

#define SHT_VERSION_STRING  "1.4.0"
#define SHT_VERSION_WSTRING L"1.4.0"
#define SHT_VERSION_COMMA   1,4,0,0

#endif
