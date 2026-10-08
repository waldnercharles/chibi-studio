#ifndef CHIBI_EDITOR_UI_KIT_ICONS_H
#define CHIBI_EDITOR_UI_KIT_ICONS_H

/* Glyphs in third_party/fonts/nerd-fonts-symbols-only, matching the Studio study's
 * icons.js. The comment names the Nerd Fonts glyph for searching the cheat sheet. */
enum
{
    KIT_ICON_ADD = 0xea60,          /* nf-cod-add */
    KIT_ICON_CLOSE = 0xea76,        /* nf-cod-close */
    KIT_ICON_CHECK = 0xeab2,        /* nf-cod-check */
    KIT_ICON_SEARCH = 0xea6d,       /* nf-cod-search */
    KIT_ICON_FILTER = 0xeaf1,       /* nf-cod-filter */
    KIT_ICON_DOWN = 0xeab4,         /* nf-cod-chevron_down */
    KIT_ICON_RIGHT = 0xeab6,        /* nf-cod-chevron_right */
    KIT_ICON_LEFT = 0xeab5,         /* nf-cod-chevron_left */
    KIT_ICON_UP = 0xeab7,           /* nf-cod-chevron_up */
    KIT_ICON_BACK = 0xea9b,         /* nf-cod-arrow_left */
    KIT_ICON_FORWARD = 0xea9c,      /* nf-cod-arrow_right */
    KIT_ICON_MORE = 0xea7c,         /* nf-cod-ellipsis */
    KIT_ICON_GRIP = 0xeb04,         /* nf-cod-gripper */
    KIT_ICON_PLAY = 0xf040a,        /* nf-md-play */
    KIT_ICON_PAUSE = 0xf03e4,       /* nf-md-pause */
    KIT_ICON_STOP = 0xf04db,        /* nf-md-stop */
    KIT_ICON_STEP = 0xf04ad,        /* nf-md-skip_next */
    KIT_ICON_RECORD = 0xf044a,      /* nf-md-record */
    KIT_ICON_REPLAY = 0xf0459,      /* nf-md-replay */
    KIT_ICON_CONTINUE = 0xeacf,     /* nf-cod-debug_continue */
    KIT_ICON_STEP_OVER = 0xead6,    /* nf-cod-debug_step_over */
    KIT_ICON_STEP_INTO = 0xead4,    /* nf-cod-debug_step_into */
    KIT_ICON_STEP_OUT = 0xead5,     /* nf-cod-debug_step_out */
    KIT_ICON_FRAME_CALL = 0xeb8b,   /* nf-cod-debug_stackframe */
    KIT_ICON_FRAME_TOP = 0xeb89,    /* nf-cod-debug_stackframe_active */
    KIT_ICON_EYE = 0xea70,          /* nf-cod-eye */
    KIT_ICON_EYE_OFF = 0xeae7,      /* nf-cod-eye_closed */
    KIT_ICON_LOCK = 0xea75,         /* nf-cod-lock */
    KIT_ICON_UNLOCK = 0xeb74,       /* nf-cod-unlock */
    KIT_ICON_SCENE = 0xebd2,        /* nf-cod-layers */
    KIT_ICON_GRAPH = 0xebb9,        /* nf-cod-type_hierarchy */
    KIT_ICON_PROTOTYPE = 0xf01a7,   /* nf-md-cube_outline */
    KIT_ICON_EVENT = 0xea86,        /* nf-cod-symbol_event */
    KIT_ICON_FOLDER = 0xea83,       /* nf-cod-folder */
    KIT_ICON_FILE = 0xea7b,         /* nf-cod-file */
    KIT_ICON_LIBRARY = 0xea8b,      /* nf-cod-symbol_namespace */
    KIT_ICON_MATERIAL = 0xf03d8,    /* nf-md-palette */
    KIT_ICON_OUTLINER = 0xeb86,     /* nf-cod-list_tree */
    KIT_ICON_PROPERTIES = 0xf062e,  /* nf-md-tune */
    KIT_ICON_VIEWPORT = 0xf0d49,    /* nf-md-axis_arrow */
    KIT_ICON_ASSETS = 0xeb29,       /* nf-cod-package */
    KIT_ICON_OUTPUT = 0xeb9d,       /* nf-cod-output */
    KIT_ICON_PHYSICS = 0xf0e7b,     /* nf-md-atom_variant */
    KIT_ICON_GAMEPLAY = 0xf04e5,    /* nf-md-sword */
    KIT_ICON_SETTINGS = 0xeaf8,     /* nf-cod-gear */
    KIT_ICON_SELECT = 0xf01bf,      /* nf-md-cursor_default_outline */
    KIT_ICON_MOVE = 0xf01be,        /* nf-md-cursor_move */
    KIT_ICON_ROTATE = 0xf0467,      /* nf-md-rotate_right */
    KIT_ICON_SCALE = 0xf0a68,       /* nf-md-resize */
    KIT_ICON_FRAME = 0xf004c,       /* nf-md-arrow_expand_all */
    KIT_ICON_PAN = 0xf0bb4,         /* nf-md-pan */
    KIT_ICON_ZOOM_IN = 0xeb81,      /* nf-cod-zoom_in */
    KIT_ICON_ZOOM_OUT = 0xeb82,     /* nf-cod-zoom_out */
    KIT_ICON_GRID = 0xf02c1,        /* nf-md-grid */
    KIT_ICON_MAGNET = 0xf0347,      /* nf-md-magnet */
    KIT_ICON_OVERLAY = 0xeb2f,      /* nf-cod-preview */
    KIT_ICON_MOUSE = 0xf037d,       /* nf-md-mouse */
    KIT_ICON_UNDO = 0xf054c,        /* nf-md-undo */
    KIT_ICON_REDO = 0xf044e,        /* nf-md-redo */
    KIT_ICON_SAVE = 0xeb4b,         /* nf-cod-save */
    KIT_ICON_HISTORY = 0xea82,      /* nf-cod-history */
    KIT_ICON_PIN = 0xf0931,         /* nf-md-pin_outline */
    KIT_ICON_EXTERNAL = 0xeb14,     /* nf-cod-link_external */
    KIT_ICON_RESET = 0xeae2,        /* nf-cod-discard */
    KIT_ICON_COPY = 0xebcc,         /* nf-cod-copy */
    KIT_ICON_CLIPBOARD = 0xf018f,   /* nf-md-content_copy */
    KIT_ICON_CUT = 0xf0190,         /* nf-md-content_cut */
    KIT_ICON_PASTE = 0xf0192,       /* nf-md-content_paste */
    KIT_ICON_DASH = 0xeacc,         /* nf-cod-dash */
    KIT_ICON_SELECTION = 0xf0135,   /* nf-md-checkbox_multiple_blank_outline */
    KIT_ICON_EDIT = 0xea73,         /* nf-cod-edit */
    KIT_ICON_TRASH = 0xea81,        /* nf-cod-trash */
    KIT_ICON_WARNING = 0xea6c,      /* nf-cod-warning */
    KIT_ICON_ERROR = 0xea87,        /* nf-cod-error */
    KIT_ICON_INFO = 0xea74,         /* nf-cod-info */
    KIT_ICON_TARGET = 0xebf8,       /* nf-cod-target */
    KIT_ICON_EYEDROPPER = 0xf020a,  /* nf-md-eyedropper */
    KIT_ICON_MAXIMIZE = 0xeb4c,     /* nf-cod-screen_full */
    KIT_ICON_FUNCTION = 0xf0871,    /* nf-md-function_variant */
    KIT_ICON_BRANCH = 0xf062c,      /* nf-md-source_branch */
    KIT_ICON_EXPRESSION = 0xf0169,  /* nf-md-code_braces */
    KIT_ICON_FIELD = 0xeb5f,        /* nf-cod-symbol_field */
    KIT_ICON_CONSTANT = 0xeb5d,     /* nf-cod-symbol_constant */
    KIT_ICON_COMPONENT = 0xf0a66,   /* nf-md-puzzle_outline */
    KIT_ICON_VARIABLE = 0xea88,     /* nf-cod-symbol_variable */
    KIT_ICON_LOCAL = 0xea92,        /* nf-cod-symbol_parameter */
    KIT_ICON_TIMER = 0xf051b,       /* nf-md-timer_outline */
    KIT_ICON_TRIANGLE = 0xf0537,    /* nf-md-triangle_outline */
    KIT_ICON_HEXAGON = 0xf02d9,     /* nf-md-hexagon_outline */
    KIT_ICON_CLOUD = 0xf0163,       /* nf-md-cloud_outline */
    KIT_ICON_WATER = 0xf0e0a,       /* nf-md-water_outline */
    KIT_ICON_CAMERA = 0xf0d5d,      /* nf-md-camera_outline */
    KIT_ICON_LIGHT = 0xf0336,       /* nf-md-lightbulb_outline */
    KIT_ICON_DIAMOND = 0xf0b8b,     /* nf-md-diamond_outline */
    KIT_ICON_CIRCLE = 0xf0766,      /* nf-md-circle_outline */
    KIT_ICON_DOT = 0xf0765,         /* nf-md-circle */
    KIT_ICON_COMMENT = 0xea6b,      /* nf-cod-comment */
    KIT_ICON_REFERENCES = 0xeb36,   /* nf-cod-references */
    KIT_ICON_CALLERS = 0xeb92,      /* nf-cod-call_incoming */
    KIT_ICON_CALLEES = 0xeb93,      /* nf-cod-call_outgoing */
    KIT_ICON_PEOPLE = 0xf0009,      /* nf-md-account_multiple */
    KIT_ICON_SYSTEM = 0xeba2,       /* nf-cod-server_process */
    KIT_ICON_TERMINAL = 0xea85,     /* nf-cod-terminal */
    KIT_ICON_SIDE_PANE = 0xebf4,    /* nf-cod-layout_sidebar_right */
    KIT_ICON_LAYOUT = 0xebeb,       /* nf-cod-layout */
    KIT_ICON_SUN = 0xf0599,         /* nf-md-weather_sunny */
    KIT_ICON_MOON = 0xf0594,        /* nf-md-weather_night */
    KIT_ICON_PASS = 0xeba4,         /* nf-cod-pass */
    KIT_ICON_LOADING = 0xeb19,      /* nf-cod-loading */
    KIT_ICON_PERFORMANCE = 0xf04c5, /* nf-md-speedometer */
    KIT_ICON_SLOWER = 0xf0cd7,      /* nf-md-turtle */
    KIT_ICON_FASTER = 0xf0907,      /* nf-md-rabbit */
    KIT_ICON_EMITTER = 0xf0674,     /* nf-md-creation */
    KIT_ICON_TERRAIN = 0xf058a,     /* nf-md-terrain */
    KIT_ICON_GAMEPAD = 0xf0297,     /* nf-md-gamepad_variant */
    KIT_ICON_KEYBOARD = 0xf097b,    /* nf-md-keyboard_outline */
    KIT_ICON_PROJECTILE = 0xf0cda,  /* nf-md-bullet */
    KIT_ICON_HEALTH = 0xf02d5,      /* nf-md-heart_outline */
    KIT_ICON_SHAPE = 0xf0832,       /* nf-md-shape_outline */

    KIT_ICON_ALIGN_TOP = 0xf11c7,    /* nf-md-align_vertical_top */
    KIT_ICON_ALIGN_MIDDLE = 0xf11c6, /* nf-md-align_vertical_center */
    KIT_ICON_ALIGN_BOTTOM = 0xf11c5, /* nf-md-align_vertical_bottom */
    KIT_ICON_ALIGN_LEFT = 0xf11c2,   /* nf-md-align_horizontal_left */
    KIT_ICON_ALIGN_CENTER = 0xf11c3, /* nf-md-align_horizontal_center */
    KIT_ICON_ALIGN_RIGHT = 0xf11c4,  /* nf-md-align_horizontal_right */
    KIT_ICON_DISTRIBUTE_X = 0xf11c9, /* nf-md-distribute_horizontal_center */
    KIT_ICON_DISTRIBUTE_Y = 0xf11cc, /* nf-md-distribute_vertical_center */
    KIT_ICON_UNFOLD = 0xeb73,        /* nf-cod-unfold */
    KIT_ICON_REFRESH = 0xeb37,       /* nf-cod-refresh */

    /* Visual designer (workstream H): UI elements by kind. */
    KIT_ICON_ELEMENT = 0xf0763,  /* nf-md-square_outline */
    KIT_ICON_TEXT = 0xf0284,     /* nf-md-format_text */
    KIT_ICON_BUTTON = 0xf12a8,   /* nf-md-gesture_tap_button */
    KIT_ICON_INPUT = 0xf060e,    /* nf-md-form_textbox */
    KIT_ICON_CHECKBOX = 0xf0a1a, /* nf-md-toggle_switch_outline */
    KIT_ICON_SLIDER = 0xf0444,   /* nf-md-ray_start_end */
    KIT_ICON_IMAGE = 0xf0976,    /* nf-md-image_outline */
    KIT_ICON_PROGRESS = 0xf029a, /* nf-md-gauge */
    KIT_ICON_WIDGET = 0xf072c,   /* nf-md-widgets */
    KIT_ICON_SLOT = 0xf1294,     /* nf-md-tray */
    KIT_ICON_WRAP = 0xf0f82,     /* nf-md-select_group */
    KIT_ICON_BINDING = 0xf0339,  /* nf-md-link_variant */

    /* Native elements for authored UI (workstream N). */
    KIT_ICON_NUMBER = 0xf0199, /* nf-md-counter */
};

#endif
