/*
 * Copyright (C) 2011-2013 Karlsruhe Institute of Technology
 *
 * This file is part of Ufo.
 *
 * This library is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <gmodule.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>
#include <glob.h>

#include "ufo-read-task.h"

#define REGION_SIZE(start, stop, step) (((stop) - (start) - 1) / (step) + 1)

typedef enum {
    TYPE_INVALID,
    TYPE_TIFF,
    TYPE_EDF,
} FileType;

struct _UfoReadTaskPrivate {
    gchar *path;
    guint count;
    guint current_count;
    guint step;
    guint start;
    guint end;
    gboolean blocking;
    gboolean normalize;
    gboolean more_pages;
    GSList *filenames;
    GSList *current_filename;

    /* General */
    FileType opened_type;
    UfoBufferDepth depth;
    guint32 width;
    guint32 height;
    guint16 bps;
    guint16 spp;
    gsize size;

    /* EDF */
    FILE *edf;
    gssize edf_file_size;
    gboolean big_endian;

    /* TIFF */
    TIFF *tiff;

    gboolean enable_conversion;

    guint roi_y;
    guint roi_height;
    guint roi_step;
};

static void ufo_task_interface_init (UfoTaskIface *iface);

G_DEFINE_TYPE_WITH_CODE (UfoReadTask, ufo_read_task, UFO_TYPE_TASK_NODE,
                         G_IMPLEMENT_INTERFACE (UFO_TYPE_TASK,
                                                ufo_task_interface_init))

#define UFO_READ_TASK_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), UFO_TYPE_READ_TASK, UfoReadTaskPrivate))

enum {
    PROP_0,
    PROP_PATH,
    PROP_BLOCKING,
    PROP_START,
    PROP_END,
    PROP_STEP,
    PROP_NORMALIZE,
    PROP_ROI_Y,
    PROP_ROI_HEIGHT,
    PROP_ROI_STEP,
    PROP_TOTAL_HEIGHT,
    PROP_ENABLE_CONVERSION,
    N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = { NULL, };

UfoNode *
ufo_read_task_new (void)
{
    return UFO_NODE (g_object_new (UFO_TYPE_READ_TASK, NULL));
}

static guint
compute_height (UfoReadTaskPrivate *priv)
{
    guint height, roi_y;

    roi_y = priv->roi_y >= priv->height ? 0 : priv->roi_y;
    if (!priv->roi_height) {
        height = priv->height - roi_y;
    }
    else {
        height = roi_y + priv->roi_height > priv->height ? priv->height - roi_y : priv->roi_height;
    }

    return height;
}

static gboolean
read_tiff_data (UfoReadTaskPrivate *priv, gpointer buffer, UfoRequisition *requisition)
{
    const guint32 width = requisition->dims[0];
    const guint32 roi_y = priv->roi_y >= priv->height ? 0 : priv->roi_y;
    const guint32 height = roi_y + compute_height (priv);
    tsize_t result;
    int offset = 0;
    int step = width;

    if (priv->bps > 8) {
        if (priv->bps <= 16)
            step *= 2;
        else
            step *= 4;
    }

    for (guint32 i = roi_y; i < height; i += priv->roi_step) {
        result = TIFFReadScanline (priv->tiff, ((gchar *) buffer) + offset, i, 0);

        if (result == -1)
            return FALSE;

        offset += step;
    }

    return TRUE;
}

static gboolean
is_tiff_file (const gchar *filename)
{
    return g_str_has_suffix (filename, ".tiff") ||
           g_str_has_suffix (filename, ".tif");
}

static gboolean
is_edf_file (const gchar *filename)
{
    return g_str_has_suffix (filename, ".edf");
}

static gboolean
has_valid_extension (const gchar *filename)
{
    return is_tiff_file (filename) || is_edf_file (filename);
}

static GSList *
read_filenames (UfoReadTaskPrivate *priv)
{
    GSList *result = NULL;
    gchar *pattern;
    glob_t glob_vector;
    guint i;
    guint end;
    guint num_globbed;

    if (!has_valid_extension (priv->path) && (g_strrstr (priv->path, "*") == NULL))
        pattern = g_build_filename (priv->path, "*", NULL);
    else
        pattern = g_strdup (priv->path);

    glob (pattern, GLOB_MARK | GLOB_TILDE, NULL, &glob_vector);
    num_globbed = (guint) glob_vector.gl_pathc;
    end = priv->end == G_MAXUINT ? num_globbed : MIN(priv->end, num_globbed);

    for (i = priv->start; i < end; i += priv->step) {
        const gchar *filename = glob_vector.gl_pathv[i];

        if (has_valid_extension (filename))
            result = g_slist_append (result, g_strdup (filename));
        else
            g_warning ("Ignoring `%s'", filename);
    }

    globfree (&glob_vector);
    g_free (pattern);
    return result;
}

static void
ufo_read_task_setup (UfoTask *task,
                       UfoResources *resources,
                       GError **error)
{
    UfoReadTask *node;
    UfoReadTaskPrivate *priv;
    guint n_files;
    guint partition;
    guint index;
    guint total;

    node = UFO_READ_TASK (task);
    priv = node->priv;

    priv->filenames = read_filenames (priv);

    if (priv->end <= priv->start) {
        g_set_error (error, UFO_TASK_ERROR, UFO_TASK_ERROR_SETUP, "End must be less than start");
        return;
    }

    if (priv->filenames == NULL) {
        g_set_error (error, UFO_TASK_ERROR, UFO_TASK_ERROR_SETUP,
                     "`%s' does not match any files", priv->path);
        return;
    }

    ufo_task_node_get_partition (UFO_TASK_NODE (task), &index, &total);
    n_files = (priv->end - priv->start - 1) / priv->step + 1;
    partition = n_files / total;
    priv->current_count = index * partition;
    priv->count = (index + 1) * partition;
    priv->current_filename = g_slist_nth (priv->filenames, (guint) priv->current_count);
}

static void
read_edf_metadata (UfoReadTaskPrivate *priv)
{
    gchar *header = g_malloc (1024);
    gchar **tokens;
    size_t num_bytes;

    num_bytes = fread (header, 1, 1024, priv->edf);

    if (num_bytes != 1024) {
        g_free (header);
        fclose (priv->edf);
        return;
    }

    tokens = g_strsplit(header, ";", 0);
    priv->big_endian = FALSE;
    priv->bps = 32;
    priv->spp = 1;

    for (guint i = 0; tokens[i] != NULL; i++) {
        gchar **key_value;
        gchar *key;
        gchar *value;

        key_value = g_strsplit (tokens[i], "=", 0);

        if (key_value[0] == NULL || key_value[1] == NULL)
            continue;

        key = g_strstrip (key_value[0]);
        value = g_strstrip (key_value[1]);

        if (!g_strcmp0 (key, "Dim_1")) {
            priv->width = (guint) atoi (value);
        }
        else if (!g_strcmp0 (key, "Dim_2")) {
            priv->height = (guint) atoi (value);
        }
        else if (!g_strcmp0 (key, "Size")) {
            priv->size = (guint) atoi (value);
        }
        else if (!g_strcmp0 (key, "DataType")) {
            if (!g_strcmp0 (value, "UnsignedShort")) {
                priv->depth = UFO_BUFFER_DEPTH_16U;
                priv->bps = 16;
            }
            else if (!g_strcmp0 (value, "SignedInteger")) {
                priv->depth = UFO_BUFFER_DEPTH_32S;
                priv->bps = 32;
            }
            else if (!g_strcmp0 (value, "UnsignedLong")) {
                /* UnsignedLong at ESRF has 32 bits */
                priv->depth = UFO_BUFFER_DEPTH_32U;
                priv->bps = 32;
            }
            else if (!g_strcmp0 (value, "Float") || !g_strcmp0 (value, "FloatValue")) {
                priv->bps = 32;
                priv->depth = UFO_BUFFER_DEPTH_32F;
            }
            else {
                g_warning ("Unsupported data type");
            }
        }
        else if (!g_strcmp0 (key, "ByteOrder") &&
                 !g_strcmp0 (value, "HighByteFirst")) {
            priv->big_endian = TRUE;
        }

        g_strfreev (key_value);
    }

    g_strfreev(tokens);
    g_free(header);
}

static gboolean
read_edf_data (UfoReadTaskPrivate *priv,
               gpointer buffer,
               UfoRequisition *requisition)
{
    gsize num_bytes;
    /* Offset to the first row */
    gssize offset;
    /* size of the image width in bytes */
    const gsize width = requisition->dims[0] * priv->bps / 8;
    const guint32 roi_y = priv->roi_y >= priv->height ? 0 : priv->roi_y;
    const guint32 height = roi_y + compute_height (priv);
    const guint num_rows = REGION_SIZE (roi_y, height, priv->roi_step);
    /* Last read row, +1 because it is actually read */
    const guint last_row = roi_y + priv->roi_step * (num_rows - 1) + 1;
    /* Position after the last image row */
    const gsize end_position = (priv->height - last_row) * width;

    offset = 0;
    /* Go to the first desired row */
    fseek (priv->edf, roi_y * width, SEEK_CUR);

    if (priv->roi_step == 1) {
        /* Read the full ROI at once if no stepping is specified */
        num_bytes = fread ((gchar *) buffer, 1, width * (height - roi_y), priv->edf);
        if (num_bytes != width * (height - roi_y)) {
            return FALSE;
        }
    }
    else {
        for (guint32 i = 0; i < num_rows - 1; i++) {
            num_bytes = fread (((gchar *) buffer) + offset, 1, width, priv->edf);

            if (num_bytes != width)
                return FALSE;

            offset += width;
            fseek (priv->edf, (priv->roi_step - 1) * width, SEEK_CUR);
        }
        /* Read the last row without moving the file pointer so that the fseek to
         * the image end works properly */
        num_bytes = fread (((gchar *) buffer) + offset, 1, width, priv->edf);
        if (num_bytes != width)
            return FALSE;
    }

    /* Go to the image end to be in a consistent state for the next read */
    fseek (priv->edf, end_position, SEEK_CUR);

    if ((G_BYTE_ORDER == G_LITTLE_ENDIAN) && priv->big_endian) {
        guint32 *data = (guint32 *) buffer;
        guint n_pixels = requisition->dims[0] * requisition->dims[1];

        for (guint i = 0; i < n_pixels; i++)
            data[i] = g_ntohl (data[i]);
    }

    return TRUE;
}

static gboolean
open_next_file (UfoReadTaskPrivate *priv)
{
    if (priv->opened_type == TYPE_EDF)
        return ftell (priv->edf) >= priv->edf_file_size;

    return TRUE;
}

static void
ufo_read_task_get_requisition (UfoTask *task,
                                 UfoBuffer **inputs,
                                 UfoRequisition *requisition)
{
    UfoReadTaskPrivate *priv;
    guint height, roi_y;

    priv = UFO_READ_TASK_GET_PRIVATE (UFO_READ_TASK (task));

    if (open_next_file (priv)) {
        if ((priv->current_count < priv->count) && (priv->current_filename != NULL)) {
            const gchar *name = (gchar *) priv->current_filename->data;

            if (is_tiff_file (name)) {
                if (priv->tiff != NULL)
                    TIFFClose (priv->tiff);

                priv->tiff = TIFFOpen (name, "r");
                priv->opened_type = TYPE_TIFF;

                TIFFGetField (priv->tiff, TIFFTAG_BITSPERSAMPLE, &priv->bps);
                TIFFGetField (priv->tiff, TIFFTAG_SAMPLESPERPIXEL, &priv->spp);
                TIFFGetField (priv->tiff, TIFFTAG_IMAGEWIDTH, &priv->width);
                TIFFGetField (priv->tiff, TIFFTAG_IMAGELENGTH, &priv->height);

                if (priv->bps == 16)
                    priv->depth = UFO_BUFFER_DEPTH_16U;
            }
            else if (is_edf_file (name)) {
                if (priv->edf != NULL) {
                    fclose (priv->edf);
                }

                priv->edf = fopen (name, "rb");

                fseek (priv->edf, 0L, SEEK_END);
                priv->edf_file_size = (gsize) ftell (priv->edf);
                fseek (priv->edf, 0L, SEEK_SET);

                priv->opened_type = TYPE_EDF;
                read_edf_metadata (priv);
            }
        }
    }
    else {
        g_assert (priv->opened_type == TYPE_EDF);
        read_edf_metadata (priv);
    }

    requisition->n_dims = 2;
    requisition->dims[0] = priv->width;

    roi_y = priv->roi_y >= priv->height ? 0 : priv->roi_y;
    height = compute_height (priv);
    requisition->dims[1] = REGION_SIZE (roi_y, roi_y + height, priv->roi_step);
}

static guint
ufo_read_task_get_num_inputs (UfoTask *task)
{
    return 0;
}

static guint
ufo_read_task_get_num_dimensions (UfoTask *task,
                               guint input)
{
    return 0;
}

static UfoTaskMode
ufo_read_task_get_mode (UfoTask *task)
{
    return UFO_TASK_MODE_GENERATOR | UFO_TASK_MODE_CPU;
}

static gboolean
ufo_read_task_generate (UfoTask *task,
                          UfoBuffer *output,
                          UfoRequisition *requisition)
{
    UfoReadTaskPrivate *priv;
    UfoProfiler *profiler;

    priv = UFO_READ_TASK_GET_PRIVATE (UFO_READ_TASK (task));
    profiler = ufo_task_node_get_profiler (UFO_TASK_NODE (task));

    if (priv->current_count < priv->count) {
        gpointer data = ufo_buffer_get_host_array (output, NULL);


        if (open_next_file (priv)) {
            if (priv->current_filename != NULL) {
                ufo_profiler_start (profiler, UFO_PROFILER_TIMER_IO);

                if (priv->tiff != NULL) {
                    read_tiff_data (priv, data, requisition);
                }
                else if (priv->edf != NULL) {
                    read_edf_data (priv, data, requisition);
                }

                ufo_profiler_stop (profiler, UFO_PROFILER_TIMER_IO);

                priv->current_filename = g_slist_next(priv->current_filename);
                priv->current_count++;
            }
            else {
                return FALSE;
            }
        }
        else {
            g_assert (priv->opened_type == TYPE_EDF);

            ufo_profiler_start (profiler, UFO_PROFILER_TIMER_IO);
            read_edf_data (priv, data, requisition);
            ufo_profiler_stop (profiler, UFO_PROFILER_TIMER_IO);

            priv->current_count++;

            if (open_next_file (priv))
                priv->current_filename = g_slist_next(priv->current_filename);
        }

        ufo_profiler_start (profiler, UFO_PROFILER_TIMER_CPU);

        if ((priv->depth != UFO_BUFFER_DEPTH_32F) && priv->enable_conversion) {
            ufo_buffer_convert (output, priv->depth);
        }

        ufo_profiler_stop (profiler, UFO_PROFILER_TIMER_CPU);
        return TRUE;
    }

    return FALSE;
}

static void
ufo_read_task_set_property (GObject *object,
                              guint property_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
    UfoReadTaskPrivate *priv = UFO_READ_TASK_GET_PRIVATE (object);

    switch (property_id) {
        case PROP_PATH:
            g_free (priv->path);
            priv->path = g_value_dup_string (value);
            break;
        case PROP_STEP:
            priv->step = g_value_get_uint (value);
            break;
        case PROP_BLOCKING:
            priv->blocking = g_value_get_boolean (value);
            break;
        case PROP_NORMALIZE:
            priv->normalize = g_value_get_boolean (value);
            break;
        case PROP_ROI_Y:
            priv->roi_y = g_value_get_uint (value);
            break;
        case PROP_ROI_HEIGHT:
            priv->roi_height = g_value_get_uint (value);
            break;
        case PROP_ROI_STEP:
            priv->roi_step = g_value_get_uint (value);
            break;
        case PROP_ENABLE_CONVERSION:
            priv->enable_conversion = g_value_get_boolean (value);
            break;
        case PROP_START:
            priv->start = g_value_get_uint (value);
            break;
        case PROP_END:
            priv->end = g_value_get_uint (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
            break;
    }
}

static void
ufo_read_task_get_property (GObject *object,
                              guint property_id,
                              GValue *value,
                              GParamSpec *pspec)
{
    UfoReadTaskPrivate *priv = UFO_READ_TASK_GET_PRIVATE (object);

    switch (property_id) {
        case PROP_PATH:
            g_value_set_string (value, priv->path);
            break;
        case PROP_STEP:
            g_value_set_uint (value, priv->step);
            break;
        case PROP_BLOCKING:
            g_value_set_boolean (value, priv->blocking);
            break;
        case PROP_NORMALIZE:
            g_value_set_boolean (value, priv->normalize);
            break;
        case PROP_ROI_Y:
            g_value_set_uint (value, priv->roi_y);
            break;
        case PROP_ROI_HEIGHT:
            g_value_set_uint (value, priv->roi_height);
            break;
        case PROP_ROI_STEP:
            g_value_set_uint (value, priv->roi_step);
            break;
        case PROP_TOTAL_HEIGHT:
            g_value_set_uint (value, priv->height);
            break;
        case PROP_ENABLE_CONVERSION:
            g_value_set_boolean (value, priv->enable_conversion);
            break;
        case PROP_START:
            g_value_set_uint (value, priv->start);
            break;
        case PROP_END:
            g_value_set_uint (value, priv->end);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
            break;
    }
}

static void
ufo_read_task_finalize (GObject *object)
{
    UfoReadTaskPrivate *priv = UFO_READ_TASK_GET_PRIVATE (object);

    g_free (priv->path);
    priv->path = NULL;

    if (priv->tiff != NULL)
        TIFFClose (priv->tiff);

    if (priv->edf != NULL)
        fclose (priv->edf);

    if (priv->filenames != NULL) {
        g_slist_foreach (priv->filenames, (GFunc) g_free, NULL);
        g_slist_free (priv->filenames);
        priv->filenames = NULL;
    }

    G_OBJECT_CLASS (ufo_read_task_parent_class)->finalize (object);
}

static void
ufo_task_interface_init (UfoTaskIface *iface)
{
    iface->setup = ufo_read_task_setup;
    iface->get_num_inputs = ufo_read_task_get_num_inputs;
    iface->get_num_dimensions = ufo_read_task_get_num_dimensions;
    iface->get_mode = ufo_read_task_get_mode;
    iface->get_requisition = ufo_read_task_get_requisition;
    iface->generate = ufo_read_task_generate;
}

static void
ufo_read_task_class_init(UfoReadTaskClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

    gobject_class->set_property = ufo_read_task_set_property;
    gobject_class->get_property = ufo_read_task_get_property;
    gobject_class->finalize = ufo_read_task_finalize;

    properties[PROP_PATH] =
        g_param_spec_string("path",
            "Glob-style pattern.",
            "Glob-style pattern that describes the file path.",
            "*.tif",
            G_PARAM_READWRITE);

    properties[PROP_STEP] =
        g_param_spec_uint("step",
        "Read every \"step\" file",
        "Read every \"step\" file",
        1, G_MAXUINT, 1,
        G_PARAM_READWRITE);

    properties[PROP_BLOCKING] =
        g_param_spec_boolean("blocking",
        "Block read",
        "Block until all files are read.",
        FALSE,
        G_PARAM_READWRITE);

    properties[PROP_NORMALIZE] =
        g_param_spec_boolean("normalize",
        "Normalize values",
        "Whether 8-bit or 16-bit values are normalized to [0.0, 1.0]",
        FALSE,
        G_PARAM_READWRITE);

    properties[PROP_ROI_Y] =
        g_param_spec_uint("y",
            "Vertical coordinate",
            "Vertical coordinate from where to start reading the image",
            0, G_MAXUINT, 0,
            G_PARAM_READWRITE);

    properties[PROP_ROI_HEIGHT] =
        g_param_spec_uint("height",
            "Height",
            "Height of the region of interest to read",
            0, G_MAXUINT, 0,
            G_PARAM_READWRITE);

    properties[PROP_ROI_STEP] =
        g_param_spec_uint("y-step",
            "Read every \"step\" row",
            "Read every \"step\" row",
            1, G_MAXUINT, 1,
            G_PARAM_READWRITE);

    properties[PROP_TOTAL_HEIGHT] =
        g_param_spec_uint("total-height",
            "Total height of an image",
            "Total height of an image",
            0, G_MAXUINT, 0,
            G_PARAM_READABLE);

    properties[PROP_ENABLE_CONVERSION] =
        g_param_spec_boolean("enable-conversion",
            "Enable automatic conversion",
            "Enable automatic conversion of input data types to float",
            TRUE,
            G_PARAM_READWRITE);

    properties[PROP_START] =
        g_param_spec_uint("start",
            "Offset to the first read file",
            "Offset to the first read file",
            0, G_MAXUINT, 0,
            G_PARAM_READWRITE);

    properties[PROP_END] =
        g_param_spec_uint("end",
            "The files will be read until \"end\" - 1 index",
            "The files will be read until \"end\" - 1 index",
            1, G_MAXUINT, G_MAXUINT,
            G_PARAM_READWRITE);

    for (guint i = PROP_0 + 1; i < N_PROPERTIES; i++)
        g_object_class_install_property (gobject_class, i, properties[i]);

    g_type_class_add_private (gobject_class, sizeof(UfoReadTaskPrivate));
}

static void
ufo_read_task_init(UfoReadTask *self)
{
    UfoReadTaskPrivate *priv = NULL;

    self->priv = priv = UFO_READ_TASK_GET_PRIVATE (self);
    priv->path = g_strdup ("*.tif");
    priv->step = 1;
    priv->blocking = FALSE;
    priv->normalize = FALSE;
    priv->more_pages = FALSE;
    priv->roi_y = 0;
    priv->roi_height = 0;
    priv->roi_step = 1;
    priv->tiff = NULL;
    priv->edf = NULL;
    priv->enable_conversion = TRUE;
    priv->start = 0;
    priv->end = G_MAXUINT;
    priv->depth = UFO_BUFFER_DEPTH_32F;
    priv->opened_type = TYPE_INVALID;
}