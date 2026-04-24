// {{NO_DEPENDENCIES}}
// For #define-ing static resources for resource script file(s).
// Used by resource.rc

// clang-format off

/* Icons */
#define IDI_MAIN                    101 /* 32x32 & 48x48 icon */
#define IDI_SMALL                   102 /* Small 16x16 icon */
#define IDI_ABOUT                   103 /* About Dialog icon */

/* Bitmaps */
#define IDB_ANTS_BMP                104 /* "Ants" icon for toolbar strip, custom icon */
#define IDB_PAUSE_BMP               105 /* "Pause" icon for toolbar strip, custom icon */
#define IDB_PLAY_BMP                106 /* "Play" icon for toolbar strip, overwrites Pause icon when needed, custom icon */
#define IDB_EXIT_BMP                107 /* "Exit" icon for toolbar strip, custom icon */
#define IDB_SOUND_BMP               108 /* "Speaker" icon for toolbar strip, same as Win2000 speaker icon*/
#define IDB_MUTE_BMP                109 /* "Mute" icon for toolbar strip, overwrites Speaker icon when needed */
#define IDB_SAVE_BMP                110 /* "Save" icon for toolbar strip, floppy disk icon */
#define IDB_TIME_BMP                111 /* "Speed" icon for toolbar strip, timer icon */

/* Main application resource, also used to attach menu */
#define IDR_MAIN                    120

/* Dialogs */
#define IDD_ABOUTDLG                130

/* Menu items */
#define IDM_ABOUT                   200
#define IDM_EXIT                    201
#define IDM_HELP                    202
#define IDM_RESERVED                203

// Save snapshot
#define IDM_SAVE_AS                 204

// Simulation control
#define IDM_PAUSED                  208 /* Pause ant simulation */

// Display mode
#define IDM_MONOCHROME              209 /* Only uses black/white background, with opposite colored ants */

// Background color choices. Ant paths are always black or white; the ant itself is magenta.
#define IDM_WHITE_BKG               210
#define IDM_BLACK_BKG               211
#define IDM_GREY_BKG                212
#define IDM_RED_BKG                 213
#define IDM_GREEN_BKG               214
#define IDM_BLUE_BKG                215

// Ant iteration speed
#define IDM_SLOW                    216
#define IDM_MEDIUM                  217
#define IDM_FAST                    218
#define IDM_HYPER                   219

// Sound settings
#define IDM_SOUND                   220

// Ant drawing threads
#define IDM_CONC_1                  221
#define IDM_CONC_2                  222
#define IDM_CONC_3                  223
#define IDM_CONC_4                  224
#define IDM_CONC_5                  225
#define IDM_CONC_6                  226
#define IDM_CONC_7                  227
#define IDM_CONC_8                  228

// Forces painting a new canvas, with whatever settings it currently has
#define IDM_REPAINT                 231

// "Single step" through ant painting, allows you to run it manually one iteration at a time
// instead of using timer
#define IDM_SINGLE                  232

// Toolbar button identifier for "Num Ants" button with submenu.
#define IDM_ANTS                    240

// Toolbar button identifier for "Speed" button with submenu.
#define IDM_SPEED                   241

// Dev menu item, test debug trap
#define IDM_TESTTRAP                250

// Timer ID for painting
#define TIMER_ANTS                  400

// Embedded background-music WAV. Loaded as a user-defined "WAVE" resource
// when kUseEmbeddedBgm is true (see utils.h). The RC file binds this ID
// to res/ants.wav; FindResourceW(L"WAVE") picks it up at runtime.
#define IDR_BGM_WAVE                500

// Custom posted-message IDs (WM_APP range, guaranteed to not clash with any
// system / common-control message). Used to defer work that mustn't run
// inside WM_CREATE — see WM_APP_AUTOPLAY usage in main.cc.
#define WM_APP_AUTOPLAY             (WM_APP + 0)

// For resources to be loaded without an ID from the system.
#ifndef IDC_STATIC
 #define IDC_STATIC                 -1
#endif // IDC_STATIC
// clang-format on
