#ifndef _SCREEN_H
#define _SCREEN_H

typedef enum _SCREEN_TYPE {
    SCREEN_NULL = 0,
    SCREEN_RGB,
    SCREEN_LVDS,
	SCREEN_MCU,
    SCREEN_TVOUT,
    SCREEN_HDMI,
} SCREEN_TYPE;

typedef enum _REFRESH_STAGE {
    REFRESH_PRE = 0,
    REFRESH_END,

} REFRESH_STAGE;


typedef enum _MCU_IOCTL {
    MCU_WRCMD = 0,
    MCU_WRDATA,
    MCU_SETBYPASS,

} MCU_IOCTL;


typedef enum _MCU_STATUS {
    MS_IDLE = 0,
    MS_MCU,
    MS_EBOOK,
    MS_EWAITSTART,
    MS_EWAITEND,
    MS_EEND,

} MCU_STATUS;

enum rk_disp_prop{       //display device property
    PRMRY = 0,                     //primary display device ,like LCD screen
    EXTEND,                        //extend display device ,like hdmi ,tv out
};

struct rk29lcd_info {
	u32 lcd_id;
	u32 txd_pin;
	u32 clk_pin;
	u32 cs_pin;
	u32	reset_pin;
	int (*io_init)(void);
	int (*io_deinit)(void);
};


/* Screen description */
typedef struct rk29fb_screen {
	/* screen type & hardware connect format & out face */
	u16 type;
	u16 hw_format;
	u16 face;
	u8 lcdc_id;    //which output interface the screeen connect to
	u8 screen_id; //screen number

	/* Screen size */
	u16 x_res;
	u16 y_res;
	u16 width;
	u16 height;

	u32 mode;
	/* Timing */
	u32 pixclock;
	u16 left_margin;
	u16 right_margin;
	u16 hsync_len;
	u16 upper_margin;
	u16 lower_margin;
	u16 vsync_len;
	u8  ft;	//the time need to display one frame,in ms
	int *dsp_lut; //display lut 
#if defined(CONFIG_HDMI_DUAL_DISP) || defined(CONFIG_ONE_LCDC_DUAL_OUTPUT_INF)
    /* Scaler mode Timing */
	u32 s_pixclock;
	u16 s_left_margin;
	u16 s_right_margin;
	u16 s_hsync_len;
	u16 s_upper_margin;
	u16 s_lower_margin;
	u16 s_vsync_len; 
	u16 s_hsync_st;
	u16 s_vsync_st;
	bool s_den_inv;
	bool s_hv_sync_inv;
	bool s_clk_inv;
#endif
	u8 hdmi_resolution;
	    /* mcu need */
	u8 mcu_wrperiod;
	u8 mcu_usefmk;
	u8 mcu_frmrate;

		/* Pin polarity */
	u8 pin_hsync;
	u8 pin_vsync;
	u8 pin_den;
	u8 pin_dclk;
	u32 lcdc_aclk;
	u8 pin_dispon;

	/* Swap rule */
	u8 swap_rb;
	u8 swap_rg;
	u8 swap_gb;
	u8 swap_delta;
	u8 swap_dumy;

	int xpos;  //horizontal display start position on the sceen ,then can be changed by application
	int ypos;
	int xsize; //horizontal and vertical display size on he screen,they can be changed by application
	int ysize;
	/* Operation function*/
	int (*init)(void);
	int (*standby)(u8 enable);
	int (*refresh)(u8 arg);
	int (*scandir)(u16 dir);
	int (*disparea)(u8 area);
	int (*sscreen_get)(struct rk29fb_screen *screen, u8 resolution);
	int (*sscreen_set)(struct rk29fb_screen *screen, bool type);// 1: use scaler 0:bypass
} rk_screen;

extern void set_lcd_info(struct rk29fb_screen *screen, struct rk29lcd_info *lcd_info);
extern void set_tv_info(struct rk29fb_screen *screen);
extern void set_hdmi_info(struct rk29fb_screen *screen);

#endif
