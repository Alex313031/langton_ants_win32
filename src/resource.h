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
#define IDB_CUSTOM_BMP              112 /* "Custom" icon for toolbar strip */
#define IDB_STOP_BMP                113 /* "Stop" icon for toolbar strip */
#define IDB_COLORS_BMP              114 /* "Colors" icon for toolbar strip */

/* Main application resource, also used to attach menu */
#define IDR_MAIN                    120

/* Dialogs */
#define IDD_ABOUTDLG                130
#define IDD_CUSTOMDLG               131
#define IDC_CUSTOMSEED              132

/* Menu items */
#define IDM_ABOUT                   200
#define IDM_EXIT                    201
#define IDM_HELP                    202
#define IDM_RESERVED                203

// Save snapshot
#define IDM_SAVE_AS                 204

// Simulation control
#define IDM_PAUSED                  208 /* Pause ant simulation */
#define IDM_STOP                    209 /* Stop ant simulation */

// Display mode
#define IDM_MONOCHROME              210 /* Only uses black/white background, with opposite colored ants */

// Background color choices. Ant paths are always black or white; the ant itself is magenta.
#define IDM_WHITE_BKG               211
#define IDM_BLACK_BKG               212
#define IDM_GREY_BKG                213
#define IDM_RED_BKG                 214
#define IDM_GREEN_BKG               215
#define IDM_BLUE_BKG                216

// Ant iteration speed
#define IDM_SLOW                    217
#define IDM_MEDIUM                  218
#define IDM_FAST                    219
#define IDM_HYPER                   220
#define IDM_REALTIME                221

// Sound settings
#define IDM_SOUND                   222

// Num. Ant drawing threads menu options
#define IDM_CONC_1                  223
#define IDM_CONC_2                  224
#define IDM_CONC_3                  225
#define IDM_CONC_4                  226
#define IDM_CONC_5                  227
#define IDM_CONC_6                  228
#define IDM_CONC_7                  229
#define IDM_CONC_8                  230
#define IDM_CONC_9                  231
#define IDM_CONC_10                 232
#define IDM_CONC_11                 233
#define IDM_CONC_12                 234
#define IDM_CONC_13                 235
#define IDM_CONC_14                 236
#define IDM_CONC_15                 237
#define IDM_CONC_16                 238
#define IDM_CONC_17                 239
#define IDM_CONC_18                 240
#define IDM_CONC_19                 241
#define IDM_CONC_20                 242
#define IDM_CONC_21                 243
#define IDM_CONC_22                 244
#define IDM_CONC_23                 245
#define IDM_CONC_24                 246
#define IDM_CONC_25                 247
#define IDM_CONC_26                 248
#define IDM_CONC_27                 249
#define IDM_CONC_28                 250
#define IDM_CONC_29                 251
#define IDM_CONC_30                 252
#define IDM_CONC_31                 253
#define IDM_CONC_32                 254

// Forces painting a new canvas, with whatever settings it currently has
#define IDM_REPAINT                 260

// "Single step" through ant painting, allows you to run it manually one iteration at a time
// instead of using timer
#define IDM_SINGLE                  261

// Toolbar button identifier for "Num Ants" button with drop down menu.
#define IDM_ANTS                    262

// Toolbar button identifier for "Speed" button with drop down menu.
#define IDM_SPEED                   263

// Toolbar button identifier for "Custom" button with drop down menu.
#define IDM_CUSTOM                  264
#define IDM_CUSTOMPLACE             265
#define IDM_CUSTOMSEED              266

// Toolbar button identifier for "Colors" button with drop down menu.
#define IDM_COLORS                  267

// Not in a UI menu, just keyboard accelerator
#define IDM_UNDO                    268

#define IDM_CYANANT                 269
#define IDM_YELLOWANT               270
#define IDM_MAGENTAANT              271
#define IDM_ALLCOLORANT             272

// Dev menu item, test debug trap
#define IDM_TESTTRAP                280

// Timer ID for painting
#define TIMER_ANTS                  300

// Embedded background-music WAV. Loaded as a user-defined "WAVE" resource
// when kUseEmbeddedBgm is true (see utils.h). The RC file binds this ID
// to res/ants.wav; FindResourceW(L"WAVE") picks it up at runtime.
#define IDR_BGM_WAVE                500

// Embedded "fahh" sound effect played by IDM_TESTTRAP right before the
// debug trap fires. Loaded via PlaySoundW(SND_RESOURCE) directly from
// the exe — no temp-file dance needed since PlaySound can read a WAVE
// resource out of memory.
#define IDR_FAHH_WAVE               501

// Custom posted-message IDs (WM_APP range, guaranteed to not clash with any
// system / common-control message). Used to defer work that mustn't run
// inside WM_CREATE — see WM_APP_AUTOPLAY usage in main.cc.
#define WM_APP_AUTOPLAY             (WM_APP + 0)

// For resources to be loaded without an ID from the system.
#ifndef IDC_STATIC
 #define IDC_STATIC                 -1
#endif // IDC_STATIC
// clang-format on
