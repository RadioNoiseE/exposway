#define FRAME_CLR                                                              \
	16, 102, 130                /* color for the frames drawn around the focused window */
#define FRAME_WDH 1.6               /* width for the frame */
#define FRAME_SEP 2                 /* seperation between the frame and the focused window */
#define TITLE_CLR                                                              \
	1, 1, 1                     /* color for the title font rendered under the window */
#define TITLE_SIZE 12               /* size for the font used to render the title */
#define DISPLAY_GAP 2.2f * TITLE_SIZE  /* gap between the display edges and the windows */
#define WIN_GRID_GAP 16             /* minimum amount of gap between windows */
#define SF_TOLERN 0.96f             /* tolerence for sf */
#define MAX_ATTMPT 60               /* max try attempts */
#define ANGLE_STEP 0.12f            /* angle iterate step */
#define INI_SEPF 0.8f               /* initial seperation factor */
#define WIN_SF_STEP 0.9f            /* window sf iterate step */
#define DEBOUNCE_DELAY_MS 400       /* key repeat */
#define DELAY_SEC 0.36              /* time to wait for grim */
