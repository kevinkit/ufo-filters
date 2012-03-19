#ifndef __UFO_FILTER_NORMALIZE_H
#define __UFO_FILTER_NORMALIZE_H

#include <glib.h>

#include <ufo/ufo-filter.h>

#define UFO_TYPE_FILTER_NORMALIZE             (ufo_filter_normalize_get_type())
#define UFO_FILTER_NORMALIZE(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), UFO_TYPE_FILTER_NORMALIZE, UfoFilterNormalize))
#define UFO_IS_FILTER_NORMALIZE(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), UFO_TYPE_FILTER_NORMALIZE))
#define UFO_FILTER_NORMALIZE_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), UFO_TYPE_FILTER_NORMALIZE, UfoFilterNormalizeClass))
#define UFO_IS_FILTER_NORMALIZE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), UFO_TYPE_FILTER_NORMALIZE))
#define UFO_FILTER_NORMALIZE_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS((obj), UFO_TYPE_FILTER_NORMALIZE, UfoFilterNormalizeClass))

typedef struct _UfoFilterNormalize           UfoFilterNormalize;
typedef struct _UfoFilterNormalizeClass      UfoFilterNormalizeClass;
typedef struct _UfoFilterNormalizePrivate    UfoFilterNormalizePrivate;

struct _UfoFilterNormalize {
    /*< private >*/
    UfoFilter parent_instance;

    UfoFilterNormalizePrivate *priv;
};

/**
 * UfoFilterNormalizeClass:
 *
 * #UfoFilterNormalize class
 */
struct _UfoFilterNormalizeClass {
    /*< private >*/
    UfoFilterClass parent_class;
};

GType ufo_filter_normalize_get_type(void);
UfoFilter *ufo_filter_plugin_new(void);

#endif
