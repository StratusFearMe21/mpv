/*
 * Chafa mpv output device implementation. Based on the Sixel video output
 *
 * Copyright (c) 2026 Isaac Mills
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>

#include <chafa.h>

#include "config.h"
#include "options/m_config.h"
#include "osdep/terminal.h"
#include "sub/osd.h"
#include "vo.h"
#include "video/sws_utils.h"
#include "video/mp_image.h"

#define TERMINAL_FALLBACK_COLS      80
#define TERMINAL_FALLBACK_ROWS      25
#define TERMINAL_FALLBACK_PX_WIDTH  320
#define TERMINAL_FALLBACK_PX_HEIGHT 240

struct vo_chafa_opts {
    int geometry_width, geometry_height;
    int pixel_mode;
    int canvas_mode;
    int dither_mode;
    int work_factor;
    int width, height, top, left;
    int pad_y, pad_x;
    int rows, cols;
    bool config_clear, alt_screen;
};

#define OPT_BASE_STRUCT struct vo_chafa_opts

static const struct m_sub_options vo_chafa_conf = {
    .opts = (const m_option_t[]) {
        {"width", OPT_INT(width)},
        {"height", OPT_INT(height)},
        {"pixel-mode", OPT_CHOICE(pixel_mode,
            {"symbols", CHAFA_PIXEL_MODE_SYMBOLS},
            {"sixels", CHAFA_PIXEL_MODE_SIXELS},
            {"kitty", CHAFA_PIXEL_MODE_KITTY},
            {"iterm2", CHAFA_PIXEL_MODE_ITERM2})},
        {"canvas-mode", OPT_CHOICE(canvas_mode,
            {"truecolor", CHAFA_CANVAS_MODE_TRUECOLOR},
            {"256", CHAFA_CANVAS_MODE_INDEXED_256},
            {"240", CHAFA_CANVAS_MODE_INDEXED_240},
            {"16", CHAFA_CANVAS_MODE_INDEXED_16},
            {"fgbg-bgfg", CHAFA_CANVAS_MODE_FGBG_BGFG},
            {"fgbg", CHAFA_CANVAS_MODE_FGBG},
            {"8", CHAFA_CANVAS_MODE_INDEXED_8},
            {"16-8", CHAFA_CANVAS_MODE_INDEXED_16_8})},
        {"dither", OPT_CHOICE(dither_mode,
            {"none", CHAFA_DITHER_MODE_NONE},
            {"ordered", CHAFA_DITHER_MODE_ORDERED},
            {"diffusion", CHAFA_DITHER_MODE_DIFFUSION},
            {"noise", CHAFA_DITHER_MODE_NOISE})},
        {"work-factor", OPT_INT(work_factor)},
        {"top", OPT_INT(top)},
        {"left", OPT_INT(left)},
        {"pad-y", OPT_INT(pad_y)},
        {"pad-x", OPT_INT(pad_x)},
        {"rows", OPT_INT(rows)},
        {"cols", OPT_INT(cols)},
        {"config-clear", OPT_BOOL(config_clear)},
        {"alt-screen", OPT_BOOL(alt_screen)},
        {0}
    },
    .size = sizeof(struct vo_chafa_opts),
    .defaults = &(const struct vo_chafa_opts){
        .pixel_mode = CHAFA_PIXEL_MODE_SYMBOLS,
        .canvas_mode = CHAFA_CANVAS_MODE_TRUECOLOR,
        .dither_mode = CHAFA_DITHER_MODE_NONE,
        .work_factor = 50,
        .pad_y = -1,
        .pad_x = -1,
        .config_clear = true,
        .alt_screen = true,
    },
    .prefix = "vo-chafa",
    .change_flags = UPDATE_VIDEO
};


struct priv {
    struct m_config_cache *opts_cache;

    ChafaCanvas *canvas;
    ChafaCanvasConfig *config;
    ChafaSymbolMap *symbol_map;
    ChafaTermInfo *term_info;
    ChafaPixelType pixel_type;
    bool skip_frame_draw;

    int left, top;  // image origin cell (1 based)
    int width, height;  // actual image px size - always reflects dst_rect.
    int width_cells, height_cells;  // actual image cells size - always reflects dst_rect.
    int num_cols, num_rows;  // terminal size in cells
    int canvas_ok;  // whether canvas vo->dwidth and vo->dheight are positive

    struct mp_rect src_rect;
    struct mp_rect dst_rect;
    struct mp_osd_res osd;
    struct mp_image *frame;
    struct mp_sws_context *sws;
};

static void dealloc_canvas_and_buffers(struct vo *vo)
{
    struct priv *priv = vo->priv;

    if (priv->canvas) {
        chafa_canvas_unref(priv->canvas);
        priv->canvas = NULL;
    }

    if (priv->config) {
        chafa_canvas_config_unref(priv->config);
        priv->config = NULL;
    }

    if (priv->symbol_map) {
        chafa_symbol_map_unref(priv->symbol_map);
        priv->symbol_map = NULL;
    }

    if (priv->term_info) {
        chafa_term_info_unref(priv->term_info);
        priv->term_info = NULL;
    }

    if (priv->frame) {
        talloc_free(priv->frame);
        priv->frame = NULL;
    }
}

static void update_canvas_dimensions(struct vo *vo)
{
    // this function sets the vo canvas size in pixels vo->dwidth, vo->dheight,
    // and the number of rows and columns available in priv->num_rows/cols
    struct priv *priv          = vo->priv;
    struct vo_chafa_opts *opts = priv->opts_cache->opts;
    int num_rows               = TERMINAL_FALLBACK_ROWS;
    int num_cols               = TERMINAL_FALLBACK_COLS;
    int total_px_width         = 0;
    int total_px_height        = 0;

    terminal_get_size2(&num_rows, &num_cols, &total_px_width, &total_px_height);

    // If the user has specified rows/cols use them for further calculations
    num_rows = (opts->rows > 0) ? opts->rows : num_rows;
    num_cols = (opts->cols > 0) ? opts->cols : num_cols;

    // If the pad value is set in between 0 and width/2 - 1, then we
    // subtract from the detected width. Otherwise, we assume that the width
    // output must be a integer multiple of num_cols and accordingly set
    // total_width to be an integer multiple of num_cols. So in case the padding
    // added by terminal is less than the number of cells in that axis, then rounding
    // down will take care of correcting the detected width and remove padding.
    if (opts->width > 0) {
        // option - set by the user, hard truth
        total_px_width = opts->width;
    } else {
        if (total_px_width <= 0) {
                // ioctl failed to read terminal width
                total_px_width = TERMINAL_FALLBACK_PX_WIDTH;
        } else {
            if (opts->pad_x >= 0 && opts->pad_x < total_px_width / 2) {
                // explicit padding set by the user
                total_px_width -= (2 * opts->pad_x);
            } else {
                // rounded "auto padding"
                total_px_width = total_px_width / num_cols * num_cols;
            }
        }
    }

    if (opts->height > 0) {
        total_px_height = opts->height;
    } else {
        if (total_px_height <= 0) {
            total_px_height = TERMINAL_FALLBACK_PX_HEIGHT;
        } else {
            if (opts->pad_y >= 0 && opts->pad_y < total_px_height / 2) {
                total_px_height -= (2 * opts->pad_y);
            } else {
                total_px_height = total_px_height / num_rows * num_rows;
            }
        }
    }

    vo->dheight = total_px_height;
    vo->dwidth  = total_px_width;

    priv->num_rows = num_rows;
    priv->num_cols = num_cols;

    priv->canvas_ok = vo->dwidth > 0 && vo->dheight > 0;
}

static inline int chafa_write(char *data, int size, void *priv)
{
    FILE *p = (FILE *)priv;
    // On POSIX platforms, write() is the fastest method. It also is the only
    // one that allows atomic writes so mpv's output will not be interrupted
    // by other processes or threads that write to stdout, which would cause
    // screen corruption. POSIX does not guarantee atomicity for writes
    // exceeding PIPE_BUF, but at least Linux does seem to implement it that
    // way.
#if HAVE_POSIX
    int remain = size;

    while (remain > 0) {
        ssize_t written = write(fileno(p), data, remain);
        if (written < 0)
            return written;
        remain -= written;
        data += written;
    }

    return size;
#else
    int ret = fwrite(data, sizeof (char), size, p);
    fflush(p);
    return ret;
#endif
}

static inline void chafa_strwrite(char *s)
{
    chafa_write(s, strlen(s), stdout);
}

static void set_chafa_output_parameters(struct vo *vo)
{
    // This function sets output scaled size in priv->width, priv->height
    // and the scaling rectangles in pixels priv->src_rect, priv->dst_rect
    // as well as image positioning in cells priv->top, priv->left.
    struct priv *priv = vo->priv;
    struct vo_chafa_opts *opts = priv->opts_cache->opts;

    vo_get_src_dst_rects(vo, &priv->src_rect, &priv->dst_rect, &priv->osd);

    priv->width  = priv->dst_rect.x1 - priv->dst_rect.x0;
    priv->height = priv->dst_rect.y1 - priv->dst_rect.y0;

    int pwidth = vo->dwidth > 0 ? vo->dwidth : 1;
    int pheight = vo->dheight > 0 ? vo->dheight : 1;

    priv->width_cells = priv->num_cols;
    priv->height_cells = priv->num_rows;

    // priv->width_cells and priv->height_cells are the width and height of dst_rect
    // and they are not changed anywhere else outside this function.
    // It is the chafa image output dimension which is output by chafa.
    chafa_calc_canvas_geometry(priv->width, priv->height, &priv->width_cells, &priv->height_cells, 0.5, TRUE, FALSE);

    // top/left values must be greater than 1. If it is set, then
    // the image will be rendered from there and no further centering is done.
    priv->top  = (opts->top  > 0) ?  opts->top :
                priv->num_rows * priv->dst_rect.y0 / pheight;
    priv->left = (opts->left > 0) ?  opts->left :
                priv->num_cols * priv->dst_rect.x0 / pwidth;
}

static int update_chafa_canvas(struct vo *vo, struct mp_image_params *params)
{
    struct priv *priv = vo->priv;
    struct vo_chafa_opts *opts = priv->opts_cache->opts;

    priv->sws->src = *params;
    priv->sws->src.w = mp_rect_w(priv->src_rect);
    priv->sws->src.h = mp_rect_h(priv->src_rect);
    priv->sws->dst = (struct mp_image_params) {
        .imgfmt = params->imgfmt,
        .w = priv->width,
        .h = priv->height,
        .p_w = 1,
        .p_h = 1,
    };

    dealloc_canvas_and_buffers(vo);

    priv->frame = mp_image_alloc(params->imgfmt, priv->width, priv->height);
    if (!priv->frame)
        return -1;

    if (mp_sws_reinit(priv->sws) < 0)
        return -1;

    priv->config = chafa_canvas_config_new();

    // Set geometry based on terminal character cells
    int canvas_width = priv->width_cells;
    int canvas_height = priv->height_cells;
    chafa_canvas_config_set_geometry(priv->config, canvas_width, canvas_height);
    chafa_canvas_config_set_cell_geometry(priv->config, priv->width / canvas_width, priv->height / canvas_height);

    if (opts->pixel_mode >= 0 && opts->pixel_mode < CHAFA_PIXEL_MODE_MAX) {
        chafa_canvas_config_set_pixel_mode(priv->config, opts->pixel_mode);
    }

    if (opts->canvas_mode >= 0 && opts->canvas_mode < CHAFA_CANVAS_MODE_MAX) {
        chafa_canvas_config_set_canvas_mode(priv->config, opts->canvas_mode);
    }

    if (opts->dither_mode >= 0 && opts->dither_mode < CHAFA_DITHER_MODE_MAX) {
        chafa_canvas_config_set_dither_mode(priv->config, opts->dither_mode);
    }

    if (opts->work_factor > 0) {
        chafa_canvas_config_set_work_factor(priv->config,
                                        (gfloat)opts->work_factor / 100.0f);
    }

    if (!priv->symbol_map) {
        priv->symbol_map = chafa_symbol_map_new();
        chafa_symbol_map_add_by_tags(priv->symbol_map, CHAFA_SYMBOL_TAG_ALL);
    }
    chafa_canvas_config_set_symbol_map(priv->config, priv->symbol_map);

    switch (params->imgfmt)
    {
        case IMGFMT_RGBA:
            priv->pixel_type = CHAFA_PIXEL_RGBA8_UNASSOCIATED;
            break;
        case IMGFMT_BGRA:
            priv->pixel_type = CHAFA_PIXEL_BGRA8_UNASSOCIATED;
            break;
        case IMGFMT_ARGB:
            priv->pixel_type = CHAFA_PIXEL_ARGB8_UNASSOCIATED;
            break;
        case IMGFMT_RGB24:
            priv->pixel_type = CHAFA_PIXEL_RGB8;
            break;
        case IMGFMT_BGR24:
            priv->pixel_type = CHAFA_PIXEL_BGR8;
            break;
        default:
            MP_ERR(vo, "Image format is not supported");
            return -1;
    }

    priv->canvas = chafa_canvas_new(priv->config);
    if (!priv->canvas) {
        MP_ERR(vo, "Failed to create Chafa canvas\n");
        return -1;
    }

    return 0;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *priv = vo->priv;
    struct vo_chafa_opts *opts = priv->opts_cache->opts;
    int ret = 0;
    update_canvas_dimensions(vo);
    if (priv->canvas_ok) {  // if too small - succeed but skip the rendering
        set_chafa_output_parameters(vo);
        ret = update_chafa_canvas(vo, params);
    }

    if (opts->config_clear)
        chafa_strwrite(TERM_ESC_CLEAR_SCREEN);
    vo->want_redraw = true;

    return ret;
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *priv = vo->priv;
    struct mp_image *mpi = NULL;

    int  prev_rows   = priv->num_rows;
    int  prev_cols   = priv->num_cols;
    int  prev_height = vo->dheight;
    int  prev_width  = vo->dwidth;
    bool resized     = false;
    bool opts_changed = m_config_cache_update(priv->opts_cache);
    update_canvas_dimensions(vo);
    if (!priv->canvas_ok)
        goto done;

    struct vo_chafa_opts *opts = priv->opts_cache->opts;

    if (opts_changed || prev_rows != priv->num_rows || prev_cols != priv->num_cols ||
        prev_width != vo->dwidth || prev_height != vo->dheight)
    {
        set_chafa_output_parameters(vo);
        // Not checking for vo->config_ok because draw_frame is never called
        // with a failed reconfig.
        if (update_chafa_canvas(vo, vo->params) < 0)
            return VO_FALSE;

        if (opts->config_clear)
            chafa_strwrite(TERM_ESC_CLEAR_SCREEN);
        resized = true;
    }

    if (frame->repeat && !frame->redraw && !resized) {
        // Frame is repeated, and no need to update OSD either
        priv->skip_frame_draw = true;
        goto done;
    } else {
        // Either frame is new, or OSD has to be redrawn
        priv->skip_frame_draw = false;
    }

    // Normal case where we have to draw the frame and the image is not NULL
    if (frame->current) {
        mpi = mp_image_new_ref(frame->current);
        struct mp_rect src_rc = priv->src_rect;
        src_rc.x0 = MP_ALIGN_DOWN(src_rc.x0, mpi->fmt.align_x);
        src_rc.y0 = MP_ALIGN_DOWN(src_rc.y0, mpi->fmt.align_y);
        mp_image_crop_rc(mpi, src_rc);

        // scale/pan to our dest rect
        mp_sws_scale(priv->sws, priv->frame, mpi);
    } else {
        // Image is NULL, so need to clear image and draw OSD
        mp_image_clear(priv->frame, 0, 0, priv->width, priv->height);
    }

    struct mp_osd_res dim = {
        .w = priv->width,
        .h = priv->height
    };
    osd_draw_on_image(vo->osd, dim, mpi ? mpi->pts : 0, 0, priv->frame);

    chafa_canvas_draw_all_pixels(priv->canvas,
                                   priv->pixel_type,
                                   priv->frame->planes[0],
                                   priv->width,
                                   priv->height,
                                   priv->frame->stride[0]);

    if (mpi)
        talloc_free(mpi);

done:
    return VO_TRUE;
}

static void flip_page(struct vo *vo)
{
    struct priv *priv = vo->priv;
    GString **output;
    gint rows = 0;

    if (!priv->canvas_ok)
        return;

    // If frame is repeated and no update required, then we skip encoding
    if (priv->skip_frame_draw)
        return;

    // Make sure that canvas and buffer are valid before drawing
    if (priv->canvas == NULL)
        return;

    chafa_canvas_print_rows(priv->canvas, priv->term_info, &output, &rows);


    for (int i = 0; output [i]; i++)
    {
        // Go to the offset row and column, then display the image
        char *pos_buf = mp_tprintf(64, TERM_ESC_GOTO_YX, priv->top + i, priv->left);
        chafa_strwrite(pos_buf);

        chafa_write(output[i]->str, output[i]->len, stdout);
    }

    chafa_free_gstring_array (output);
}

static int preinit(struct vo *vo)
{
    struct priv *priv = vo->priv;

    priv->opts_cache = m_config_cache_alloc(priv, vo->global, &vo_chafa_conf);

    struct vo_chafa_opts *opts = priv->opts_cache->opts;

    // Parse opts set by CLI or conf
    priv->sws = mp_sws_alloc(vo);
    priv->sws->log = vo->log;
    mp_sws_enable_cmdline_opts(priv->sws, vo->global);

    if (opts->alt_screen)
        chafa_strwrite(TERM_ESC_ALT_SCREEN);

    chafa_strwrite(TERM_ESC_HIDE_CURSOR);
    terminal_set_mouse_input(true);

    priv->canvas = NULL;
    priv->config = NULL;
    priv->symbol_map = NULL;

    gchar **envp = g_get_environ();
    priv->term_info = chafa_term_db_detect(chafa_term_db_get_default (), envp);
    g_strfreev (envp);

    // Comment from Chafa repo
    /* Chafa may create and destroy GThreadPools multiple times while rendering
     * an image. This reduces thread churn and saves a decent amount of CPU. */
    g_thread_pool_set_max_unused_threads (-1);

    return 0;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_RGBA
        || format == IMGFMT_BGRA
        || format == IMGFMT_ARGB
        || format == IMGFMT_RGB24
        || format == IMGFMT_BGR24;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    if (request == VOCTRL_SET_PANSCAN)
        return (vo->config_ok && !reconfig(vo, vo->params)) ? VO_TRUE : VO_FALSE;
    return VO_NOTIMPL;
}


static void uninit(struct vo *vo)
{
    struct priv *priv = vo->priv;
    struct vo_chafa_opts *opts = priv->opts_cache->opts;

    chafa_strwrite(TERM_ESC_RESTORE_CURSOR);
    terminal_set_mouse_input(false);

    if (opts->alt_screen)
        chafa_strwrite(TERM_ESC_NORMAL_SCREEN);
    fflush(stdout);

    dealloc_canvas_and_buffers(vo);
}

const struct vo_driver video_out_chafa = {
    .name = "chafa",
    .description = "terminal graphics using Chafa",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .global_opts = &vo_chafa_conf,
};
