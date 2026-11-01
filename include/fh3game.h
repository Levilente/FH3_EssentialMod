/*
 * fh3game.h -- names for places in Forza Horizon 3's code and data (MIT licensed, see LICENSE).
 *
 * Azimuth runs the Xbox One build of the game: forza_durango_release_final.exe of 5 June 2018
 * (TimeDateStamp 0x5B16ECCA, SizeOfImage 0x5495000), mapped at 0x140000000. Every FH3_GAME_... below is
 * an offset from the game's base, the address wait_for_code() returns. FH3_GAME_..._BYTES is what the
 * game has there on disk, as a string, for a check before you hook or use it:
 *
 *     char* game = (char*)api->wait_for_code(30000);
 *     if (game && memcmp(game + FH3_GAME_FORZACAR_VTABLE, FH3_GAME_FORZACAR_VTABLE_BYTES,
 *                        sizeof FH3_GAME_FORZACAR_VTABLE_BYTES - 1) == 0) { ... }
 *
 * Unlike FH2, FH3's vtables are in the .data section, not .rdata.
 *
 * Everything in this file was read from the exe on disk (RTTI), none of it has been run in the game yet.
 * Names ending in _VTABLE are a vtable (its bytes are the address of slot 0 as mapped at 0x140000000).
 * The FH2 header's function offsets do not apply to this game.
 */
#ifndef FH3GAME_H
#define FH3GAME_H

#define FH3_GAME_TIMESTAMP   0x5B16ECCA
#define FH3_GAME_SIZEOFIMAGE 0x5495000

/* ---- Vtables ------------------------------------------------------------------------------------- */

/* File system: a list of mounted volumes (IOSys::CMultiVolume). slot 0 is at game+0x1C43FC0. */
#define FH3_GAME_MULTIVOLUME_VTABLE 0x4678750
#define FH3_GAME_MULTIVOLUME_VTABLE_BYTES "\xC0\x3F\xC4\x41\x01\x00\x00\x00"

/* File system: one folder or file volume (IOSys::CFileVolume). slot 0 is at game+0x1C43F40. */
#define FH3_GAME_FILEVOLUME_VTABLE 0x4678558
#define FH3_GAME_FILEVOLUME_VTABLE_BYTES "\x40\x3F\xC4\x41\x01\x00\x00\x00"

/* File system: a zip volume (IOSys::CCompressedVolume). slot 0 is at game+0x1C43D00. */
#define FH3_GAME_COMPRESSEDVOLUME_VTABLE 0x467A1D8
#define FH3_GAME_COMPRESSEDVOLUME_VTABLE_BYTES "\x00\x3D\xC4\x41\x01\x00\x00\x00"

/* Game text: the string table manager. slot 0 is at game+0x1CB38B0. */
#define FH3_GAME_STRINGTABLEMANAGER_VTABLE 0x467D858
#define FH3_GAME_STRINGTABLEMANAGER_VTABLE_BYTES "\xB0\x38\xCB\x41\x01\x00\x00\x00"

/* Game text: a localised string reference. slot 0 is at game+0x1517D20. */
#define FH3_GAME_LOCSTRING_VTABLE 0x467D8D0
#define FH3_GAME_LOCSTRING_VTABLE_BYTES "\x20\x7D\x51\x41\x01\x00\x00\x00"

/* Title screen page (A_Splash.xaml). slot 0 is at game+0x1686180. */
#define FH3_GAME_SPLASH_VTABLE 0x45DC898
#define FH3_GAME_SPLASH_VTABLE_BYTES "\x80\x61\x68\x41\x01\x00\x00\x00"

/* Pause > Settings page (A_PauseOptions.xaml). slot 0 is at game+0x156C300. */
#define FH3_GAME_PAUSEOPTIONSMENU_VTABLE 0x458D008
#define FH3_GAME_PAUSEOPTIONSMENU_VTABLE_BYTES "\x00\xC3\x56\x41\x01\x00\x00\x00"

/* Game options page. slot 0 is at game+0x165B970. */
#define FH3_GAME_GAMEOPTIONSMENU_VTABLE 0x4563CB0
#define FH3_GAME_GAMEOPTIONSMENU_VTABLE_BYTES "\x70\xB9\x65\x41\x01\x00\x00\x00"

/* Message box popup (A_MessageBox.xaml). slot 0 is at game+0x112A070. */
#define FH3_GAME_POPUPMESSAGEBOX_VTABLE 0x45AC778
#define FH3_GAME_POPUPMESSAGEBOX_VTABLE_BYTES "\x70\xA0\x12\x41\x01\x00\x00\x00"

/* ForzaCar (primary vtable). slot 0 is at game+0x3158C0. */
#define FH3_GAME_FORZACAR_VTABLE 0x42E0650
#define FH3_GAME_FORZACAR_VTABLE_BYTES "\xC0\x58\x31\x40\x01\x00\x00\x00"

/* The free-look (photo mode) camera. slot 0 is at game+0x10B0500. */
#define FH3_GAME_FREELOOKCAMERA_VTABLE 0x4497038
#define FH3_GAME_FREELOOKCAMERA_VTABLE_BYTES "\x00\x05\x0B\x41\x01\x00\x00\x00"

/* Command-line parameters (builds "%scmdline.txt"). slot 0 is at game+0x3156C0. */
#define FH3_GAME_COMMANDLINE_VTABLE 0x42E03A8
#define FH3_GAME_COMMANDLINE_VTABLE_BYTES "\xC0\x56\x31\x40\x01\x00\x00\x00"

/* The radio's FMOD stream. slot 0 is at game+0x196A800. */
#define FH3_GAME_RADIOSTREAMFMOD_VTABLE 0x4638CB8
#define FH3_GAME_RADIOSTREAMFMOD_VTABLE_BYTES "\x00\xA8\x96\x41\x01\x00\x00\x00"

#endif /* FH3GAME_H */
