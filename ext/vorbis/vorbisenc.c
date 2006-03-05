/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-vorbisenc
 * @short_description: an encoder that encodes audio to Vorbis
 * @see_also: vorbisdec, oggmux
 *
 * <refsect2>
 * <para>
 * This element encodes raw float audio into a Vorbis stream.
 * <ulink url="http://www.vorbis.com/">Vorbis</ulink> is a royalty-free
 * audio codec maintained by the <ulink url="http://www.xiph.org/">Xiph.org
 * Foundation</ulink>.
 * </para>
 * <title>Example pipelines</title>
 * <para>
 * Encode a test sine signal to Ogg/Vorbis.  Note that the resulting file
 * will be really small because a sine signal compresses very well.
 * </para>
 * <programlisting>
 * gst-launch -v audiotestsrc wave=sine num-buffers=100 ! audioconvert ! vorbisenc ! oggmux ! filesink location=sine.ogg
 * </programlisting>
 * <para>
 * Record from a sound card using ALSA and encode to Ogg/Vorbis.
 * </para>
 * <programlisting>
 * gst-launch -v alsasrc ! audioconvert ! vorbisenc ! oggmux ! filesink location=alsasrc.ogg
 * </programlisting>
 * </refsect2>
 *
 * Last reviewed on 2006-03-01 (0.10.4)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vorbis/vorbisenc.h>

#include <gst/gsttagsetter.h>
#include <gst/tag/tag.h>
#include "vorbisenc.h"

GST_DEBUG_CATEGORY_EXTERN (vorbisenc_debug);
#define GST_CAT_DEFAULT vorbisenc_debug

static GstPadTemplate *gst_vorbisenc_src_template, *gst_vorbisenc_sink_template;

/* elementfactory information */
GstElementDetails vorbisenc_details = {
  "Vorbis encoder",
  "Codec/Encoder/Audio",
  "Encodes audio in Vorbis format",
  "Monty <monty@xiph.org>, " "Wim Taymans <wim@fluendo.com>",
};

/* GstVorbisEnc signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_0,
  ARG_MAX_BITRATE,
  ARG_BITRATE,
  ARG_MIN_BITRATE,
  ARG_QUALITY,
  ARG_MANAGED,
  ARG_LAST_MESSAGE
};

static GstFlowReturn gst_vorbisenc_output_buffers (GstVorbisEnc * vorbisenc);

/* this function takes into account the granulepos_offset and the subgranule
 * time offset */
static GstClockTime
granulepos_to_timestamp_offset (GstVorbisEnc * vorbisenc,
    ogg_int64_t granulepos)
{
  if (granulepos >= 0)
    return gst_util_uint64_scale ((guint64) granulepos
        + vorbisenc->granulepos_offset, GST_SECOND, vorbisenc->frequency)
        + vorbisenc->subgranule_offset;
  return GST_CLOCK_TIME_NONE;
}

/* this function does a straight granulepos -> timestamp conversion */
static GstClockTime
granulepos_to_timestamp (GstVorbisEnc * vorbisenc, ogg_int64_t granulepos)
{
  if (granulepos >= 0)
    return gst_util_uint64_scale ((guint64) granulepos,
        GST_SECOND, vorbisenc->frequency);
  return GST_CLOCK_TIME_NONE;
}

#if 0
static const GstFormat *
gst_vorbisenc_get_formats (GstPad * pad)
{
  static const GstFormat src_formats[] = {
    GST_FORMAT_BYTES,
    GST_FORMAT_TIME,
    0
  };
  static const GstFormat sink_formats[] = {
    GST_FORMAT_BYTES,
    GST_FORMAT_DEFAULT,
    GST_FORMAT_TIME,
    0
  };

  return (GST_PAD_IS_SRC (pad) ? src_formats : sink_formats);
}
#endif

#define MAX_BITRATE_DEFAULT     -1
#define BITRATE_DEFAULT         -1
#define MIN_BITRATE_DEFAULT     -1
#define QUALITY_DEFAULT         0.3
#define LOWEST_BITRATE          6000    /* lowest allowed for a 8 kHz stream */
#define HIGHEST_BITRATE         250001  /* highest allowed for a 44 kHz stream */

static void gst_vorbisenc_base_init (gpointer g_class);
static void gst_vorbisenc_class_init (GstVorbisEncClass * klass);
static void gst_vorbisenc_init (GstVorbisEnc * vorbisenc);

static gboolean gst_vorbisenc_sink_event (GstPad * pad, GstEvent * event);
static GstFlowReturn gst_vorbisenc_chain (GstPad * pad, GstBuffer * buffer);
static gboolean gst_vorbisenc_setup (GstVorbisEnc * vorbisenc);

static void gst_vorbisenc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_vorbisenc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static GstStateChangeReturn gst_vorbisenc_change_state (GstElement * element,
    GstStateChange transition);

static GstElementClass *parent_class = NULL;

/*static guint gst_vorbisenc_signals[LAST_SIGNAL] = { 0 }; */

GType
vorbisenc_get_type (void)
{
  static GType vorbisenc_type = 0;

  if (!vorbisenc_type) {
    static const GTypeInfo vorbisenc_info = {
      sizeof (GstVorbisEncClass),
      gst_vorbisenc_base_init,
      NULL,
      (GClassInitFunc) gst_vorbisenc_class_init,
      NULL,
      NULL,
      sizeof (GstVorbisEnc),
      0,
      (GInstanceInitFunc) gst_vorbisenc_init,
    };
    static const GInterfaceInfo tag_setter_info = {
      NULL,
      NULL,
      NULL
    };

    vorbisenc_type =
        g_type_register_static (GST_TYPE_ELEMENT, "GstVorbisEnc",
        &vorbisenc_info, 0);

    g_type_add_interface_static (vorbisenc_type, GST_TYPE_TAG_SETTER,
        &tag_setter_info);
  }
  return vorbisenc_type;
}

static GstCaps *
vorbis_caps_factory (void)
{
  return gst_caps_new_simple ("audio/x-vorbis", NULL);
}

static GstCaps *
raw_caps_factory (void)
{
  /* lowest sample rate is in vorbis/lib/modes/setup_8.h, 8000 Hz
   * highest sample rate is in vorbis/lib/modes/setup_44.h, 50000 Hz */
  return
      gst_caps_new_simple ("audio/x-raw-float",
      "rate", GST_TYPE_INT_RANGE, 8000, 50000,
      "channels", GST_TYPE_INT_RANGE, 1, 2,
      "endianness", G_TYPE_INT, G_BYTE_ORDER, "width", G_TYPE_INT, 32, NULL);
}

static void
gst_vorbisenc_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstCaps *raw_caps, *vorbis_caps;

  raw_caps = raw_caps_factory ();
  vorbis_caps = vorbis_caps_factory ();

  gst_vorbisenc_sink_template = gst_pad_template_new ("sink", GST_PAD_SINK,
      GST_PAD_ALWAYS, raw_caps);
  gst_vorbisenc_src_template = gst_pad_template_new ("src", GST_PAD_SRC,
      GST_PAD_ALWAYS, vorbis_caps);
  gst_element_class_add_pad_template (element_class,
      gst_vorbisenc_sink_template);
  gst_element_class_add_pad_template (element_class,
      gst_vorbisenc_src_template);
  gst_element_class_set_details (element_class, &vorbisenc_details);
}

static void
gst_vorbisenc_class_init (GstVorbisEncClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_vorbisenc_set_property;
  gobject_class->get_property = gst_vorbisenc_get_property;

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_MAX_BITRATE,
      g_param_spec_int ("max-bitrate", "Maximum Bitrate",
          "Specify a maximum bitrate (in bps). Useful for streaming "
          "applications. (-1 == disabled)",
          -1, HIGHEST_BITRATE, MAX_BITRATE_DEFAULT, G_PARAM_READWRITE));
  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_BITRATE,
      g_param_spec_int ("bitrate", "Target Bitrate",
          "Attempt to encode at a bitrate averaging this (in bps). "
          "This uses the bitrate management engine, and is not recommended for most users. "
          "Quality is a better alternative. (-1 == disabled)",
          -1, HIGHEST_BITRATE, BITRATE_DEFAULT, G_PARAM_READWRITE));
  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_MIN_BITRATE,
      g_param_spec_int ("min_bitrate", "Minimum Bitrate",
          "Specify a minimum bitrate (in bps). Useful for encoding for a "
          "fixed-size channel. (-1 == disabled)",
          -1, HIGHEST_BITRATE, MIN_BITRATE_DEFAULT, G_PARAM_READWRITE));
  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_QUALITY,
      g_param_spec_float ("quality", "Quality",
          "Specify quality instead of specifying a particular bitrate.",
          -0.1, 1.0, QUALITY_DEFAULT, G_PARAM_READWRITE));
  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_MANAGED,
      g_param_spec_boolean ("managed", "Managed",
          "Enable bitrate management engine", FALSE, G_PARAM_READWRITE));
  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_LAST_MESSAGE,
      g_param_spec_string ("last-message", "last-message",
          "The last status message", NULL, G_PARAM_READABLE));

  parent_class = g_type_class_ref (GST_TYPE_ELEMENT);

  gstelement_class->change_state = gst_vorbisenc_change_state;
}

static gboolean
gst_vorbisenc_sink_setcaps (GstPad * pad, GstCaps * caps)
{
  GstVorbisEnc *vorbisenc;
  GstStructure *structure;

  vorbisenc = GST_VORBISENC (GST_PAD_PARENT (pad));
  vorbisenc->setup = FALSE;

  structure = gst_caps_get_structure (caps, 0);
  gst_structure_get_int (structure, "channels", &vorbisenc->channels);
  gst_structure_get_int (structure, "rate", &vorbisenc->frequency);

  gst_vorbisenc_setup (vorbisenc);

  if (vorbisenc->setup)
    return TRUE;

  return FALSE;
}

static gboolean
gst_vorbisenc_convert_src (GstPad * pad, GstFormat src_format, gint64 src_value,
    GstFormat * dest_format, gint64 * dest_value)
{
  gboolean res = TRUE;
  GstVorbisEnc *vorbisenc;
  gint64 avg;

  vorbisenc = GST_VORBISENC (gst_pad_get_parent (pad));

  if (vorbisenc->samples_in == 0 ||
      vorbisenc->bytes_out == 0 || vorbisenc->frequency == 0)
    return FALSE;

  avg = (vorbisenc->bytes_out * vorbisenc->frequency) / (vorbisenc->samples_in);

  switch (src_format) {
    case GST_FORMAT_BYTES:
      switch (*dest_format) {
        case GST_FORMAT_TIME:
          *dest_value = gst_util_uint64_scale_int (src_value, GST_SECOND, avg);
          break;
        default:
          res = FALSE;
      }
      break;
    case GST_FORMAT_TIME:
      switch (*dest_format) {
        case GST_FORMAT_BYTES:
          *dest_value = gst_util_uint64_scale_int (src_value, avg, GST_SECOND);
          break;
        default:
          res = FALSE;
      }
      break;
    default:
      res = FALSE;
  }
  return res;
}

static gboolean
gst_vorbisenc_convert_sink (GstPad * pad, GstFormat src_format,
    gint64 src_value, GstFormat * dest_format, gint64 * dest_value)
{
  gboolean res = TRUE;
  guint scale = 1;
  gint bytes_per_sample;
  GstVorbisEnc *vorbisenc;

  vorbisenc = GST_VORBISENC (gst_pad_get_parent (pad));

  bytes_per_sample = vorbisenc->channels * 2;

  switch (src_format) {
    case GST_FORMAT_BYTES:
      switch (*dest_format) {
        case GST_FORMAT_DEFAULT:
          if (bytes_per_sample == 0)
            return FALSE;
          *dest_value = src_value / bytes_per_sample;
          break;
        case GST_FORMAT_TIME:
        {
          gint byterate = bytes_per_sample * vorbisenc->frequency;

          if (byterate == 0)
            return FALSE;
          *dest_value =
              gst_util_uint64_scale_int (src_value, GST_SECOND, byterate);
          break;
        }
        default:
          res = FALSE;
      }
      break;
    case GST_FORMAT_DEFAULT:
      switch (*dest_format) {
        case GST_FORMAT_BYTES:
          *dest_value = src_value * bytes_per_sample;
          break;
        case GST_FORMAT_TIME:
          if (vorbisenc->frequency == 0)
            return FALSE;
          *dest_value =
              gst_util_uint64_scale_int (src_value, GST_SECOND,
              vorbisenc->frequency);
          break;
        default:
          res = FALSE;
      }
      break;
    case GST_FORMAT_TIME:
      switch (*dest_format) {
        case GST_FORMAT_BYTES:
          scale = bytes_per_sample;
          /* fallthrough */
        case GST_FORMAT_DEFAULT:
          *dest_value =
              gst_util_uint64_scale_int (src_value,
              scale * vorbisenc->frequency, GST_SECOND);
          break;
        default:
          res = FALSE;
      }
      break;
    default:
      res = FALSE;
  }
  return res;
}

static const GstQueryType *
gst_vorbisenc_get_query_types (GstPad * pad)
{
  static const GstQueryType gst_vorbisenc_src_query_types[] = {
    GST_QUERY_POSITION,
    GST_QUERY_DURATION,
    GST_QUERY_CONVERT,
    0
  };

  return gst_vorbisenc_src_query_types;
}

static gboolean
gst_vorbisenc_src_query (GstPad * pad, GstQuery * query)
{
  gboolean res = TRUE;
  GstVorbisEnc *vorbisenc;
  GstPad *peerpad;

  vorbisenc = GST_VORBISENC (gst_pad_get_parent (pad));
  peerpad = gst_pad_get_peer (GST_PAD (vorbisenc->sinkpad));

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:
    {
      GstFormat fmt, req_fmt;
      gint64 pos, val;

      gst_query_parse_position (query, &req_fmt, NULL);
      if ((res = gst_pad_query_position (peerpad, &req_fmt, &val))) {
        gst_query_set_position (query, req_fmt, val);
        break;
      }

      fmt = GST_FORMAT_TIME;
      if (!(res = gst_pad_query_position (peerpad, &fmt, &pos)))
        break;

      if ((res = gst_pad_query_convert (peerpad, fmt, pos, &req_fmt, &val))) {
        gst_query_set_position (query, req_fmt, val);
      }
      break;
    }
    case GST_QUERY_DURATION:
    {
      GstFormat fmt, req_fmt;
      gint64 dur, val;

      gst_query_parse_duration (query, &req_fmt, NULL);
      if ((res = gst_pad_query_duration (peerpad, &req_fmt, &val))) {
        gst_query_set_duration (query, req_fmt, val);
        break;
      }

      fmt = GST_FORMAT_TIME;
      if (!(res = gst_pad_query_duration (peerpad, &fmt, &dur)))
        break;

      if ((res = gst_pad_query_convert (peerpad, fmt, dur, &req_fmt, &val))) {
        gst_query_set_duration (query, req_fmt, val);
      }
      break;
    }
    case GST_QUERY_CONVERT:
    {
      GstFormat src_fmt, dest_fmt;
      gint64 src_val, dest_val;

      gst_query_parse_convert (query, &src_fmt, &src_val, &dest_fmt, &dest_val);
      if (!(res =
              gst_vorbisenc_convert_src (pad, src_fmt, src_val, &dest_fmt,
                  &dest_val)))
        goto error;
      gst_query_set_convert (query, src_fmt, src_val, dest_fmt, dest_val);
      break;
    }
    default:
      res = gst_pad_query_default (pad, query);
      break;
  }

error:
  gst_object_unref (peerpad);
  gst_object_unref (vorbisenc);
  return res;
}

static gboolean
gst_vorbisenc_sink_query (GstPad * pad, GstQuery * query)
{
  gboolean res = TRUE;
  GstVorbisEnc *vorbisenc;

  vorbisenc = GST_VORBISENC (GST_PAD_PARENT (pad));

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONVERT:
    {
      GstFormat src_fmt, dest_fmt;
      gint64 src_val, dest_val;

      gst_query_parse_convert (query, &src_fmt, &src_val, &dest_fmt, &dest_val);
      if (!(res =
              gst_vorbisenc_convert_sink (pad, src_fmt, src_val, &dest_fmt,
                  &dest_val)))
        goto error;
      gst_query_set_convert (query, src_fmt, src_val, dest_fmt, dest_val);
      break;
    }
    default:
      res = gst_pad_query_default (pad, query);
      break;
  }

error:
  return res;
}

static void
gst_vorbisenc_init (GstVorbisEnc * vorbisenc)
{
  vorbisenc->sinkpad =
      gst_pad_new_from_template (gst_vorbisenc_sink_template, "sink");
  gst_element_add_pad (GST_ELEMENT (vorbisenc), vorbisenc->sinkpad);
  gst_pad_set_event_function (vorbisenc->sinkpad, gst_vorbisenc_sink_event);
  gst_pad_set_chain_function (vorbisenc->sinkpad, gst_vorbisenc_chain);
  gst_pad_set_setcaps_function (vorbisenc->sinkpad, gst_vorbisenc_sink_setcaps);
  gst_pad_set_query_function (vorbisenc->sinkpad,
      GST_DEBUG_FUNCPTR (gst_vorbisenc_sink_query));

  vorbisenc->srcpad =
      gst_pad_new_from_template (gst_vorbisenc_src_template, "src");
  gst_pad_set_query_function (vorbisenc->srcpad,
      GST_DEBUG_FUNCPTR (gst_vorbisenc_src_query));
  gst_pad_set_query_type_function (vorbisenc->srcpad,
      GST_DEBUG_FUNCPTR (gst_vorbisenc_get_query_types));
  gst_element_add_pad (GST_ELEMENT (vorbisenc), vorbisenc->srcpad);

  vorbisenc->channels = -1;
  vorbisenc->frequency = -1;

  vorbisenc->managed = FALSE;
  vorbisenc->max_bitrate = MAX_BITRATE_DEFAULT;
  vorbisenc->bitrate = BITRATE_DEFAULT;
  vorbisenc->min_bitrate = MIN_BITRATE_DEFAULT;
  vorbisenc->quality = QUALITY_DEFAULT;
  vorbisenc->quality_set = FALSE;
  vorbisenc->last_message = NULL;
}


static gchar *
gst_vorbisenc_get_tag_value (const GstTagList * list, const gchar * tag,
    int index)
{
  GType tag_type;
  gchar *vorbisvalue = NULL;

  if (tag == NULL)
    return NULL;

  tag_type = gst_tag_get_type (tag);

  /* get tag name right */
  if ((strcmp (tag, GST_TAG_TRACK_NUMBER) == 0)
      || (strcmp (tag, GST_TAG_ALBUM_VOLUME_NUMBER) == 0)
      || (strcmp (tag, GST_TAG_TRACK_COUNT) == 0)
      || (strcmp (tag, GST_TAG_ALBUM_VOLUME_COUNT) == 0)) {
    guint track_no;

    if (!gst_tag_list_get_uint_index (list, tag, index, &track_no))
      g_return_val_if_reached (NULL);

    vorbisvalue = g_strdup_printf ("%u", track_no);
  } else if (tag_type == GST_TYPE_DATE) {
    GDate *date;

    if (!gst_tag_list_get_date_index (list, tag, index, &date))
      g_return_val_if_reached (NULL);

    vorbisvalue =
        g_strdup_printf ("%04d-%02d-%02d", (gint) g_date_get_year (date),
        (gint) g_date_get_month (date), (gint) g_date_get_day (date));
    g_date_free (date);
  } else if (tag_type == G_TYPE_STRING) {
    if (!gst_tag_list_get_string_index (list, tag, index, &vorbisvalue))
      g_return_val_if_reached (NULL);
  }

  return vorbisvalue;
}

static void
gst_vorbisenc_metadata_set1 (const GstTagList * list, const gchar * tag,
    gpointer vorbisenc)
{
  const gchar *vorbistag = NULL;
  gchar *vorbisvalue = NULL;
  guint i, count;
  GstVorbisEnc *enc = GST_VORBISENC (vorbisenc);

  vorbistag = gst_tag_to_vorbis_tag (tag);
  if (vorbistag == NULL) {
    return;
  }

  count = gst_tag_list_get_tag_size (list, tag);
  for (i = 0; i < count; i++) {
    vorbisvalue = gst_vorbisenc_get_tag_value (list, tag, i);

    if (vorbisvalue != NULL) {
      gchar *tmptag = g_strdup (vorbistag);

      vorbis_comment_add_tag (&enc->vc, tmptag, vorbisvalue);
      g_free (tmptag);
      g_free (vorbisvalue);
    }
  }
}

static void
gst_vorbisenc_set_metadata (GstVorbisEnc * vorbisenc)
{
  GstTagList *copy;
  const GstTagList *user_tags;

  user_tags = gst_tag_setter_get_tag_list (GST_TAG_SETTER (vorbisenc));
  if (!(vorbisenc->tags || user_tags))
    return;

  copy =
      gst_tag_list_merge (user_tags, vorbisenc->tags,
      gst_tag_setter_get_tag_merge_mode (GST_TAG_SETTER (vorbisenc)));
  vorbis_comment_init (&vorbisenc->vc);
  gst_tag_list_foreach (copy, gst_vorbisenc_metadata_set1, vorbisenc);
  gst_tag_list_free (copy);
}

static gchar *
get_constraints_string (GstVorbisEnc * vorbisenc)
{
  gint min = vorbisenc->min_bitrate;
  gint max = vorbisenc->max_bitrate;
  gchar *result;

  if (min > 0 && max > 0)
    result = g_strdup_printf ("(min %d bps, max %d bps)", min, max);
  else if (min > 0)
    result = g_strdup_printf ("(min %d bps, no max)", min);
  else if (max > 0)
    result = g_strdup_printf ("(no min, max %d bps)", max);
  else
    result = g_strdup_printf ("(no min or max)");

  return result;
}

static void
update_start_message (GstVorbisEnc * vorbisenc)
{
  gchar *constraints;

  g_free (vorbisenc->last_message);

  if (vorbisenc->bitrate > 0) {
    if (vorbisenc->managed) {
      constraints = get_constraints_string (vorbisenc);
      vorbisenc->last_message =
          g_strdup_printf ("encoding at average bitrate %d bps %s",
          vorbisenc->bitrate, constraints);
      g_free (constraints);
    } else {
      vorbisenc->last_message =
          g_strdup_printf
          ("encoding at approximate bitrate %d bps (VBR encoding enabled)",
          vorbisenc->bitrate);
    }
  } else {
    if (vorbisenc->quality_set) {
      if (vorbisenc->managed) {
        constraints = get_constraints_string (vorbisenc);
        vorbisenc->last_message =
            g_strdup_printf
            ("encoding at quality level %2.2f using constrained VBR %s",
            vorbisenc->quality, constraints);
        g_free (constraints);
      } else {
        vorbisenc->last_message =
            g_strdup_printf ("encoding at quality level %2.2f",
            vorbisenc->quality);
      }
    } else {
      constraints = get_constraints_string (vorbisenc);
      vorbisenc->last_message =
          g_strdup_printf ("encoding using bitrate management %s", constraints);
      g_free (constraints);
    }
  }

  g_object_notify (G_OBJECT (vorbisenc), "last_message");
}

static gboolean
gst_vorbisenc_setup (GstVorbisEnc * vorbisenc)
{
  vorbisenc->setup = FALSE;

  if (vorbisenc->bitrate < 0 && vorbisenc->min_bitrate < 0
      && vorbisenc->max_bitrate < 0) {
    vorbisenc->quality_set = TRUE;
  }

  update_start_message (vorbisenc);

  /* choose an encoding mode */
  /* (mode 0: 44kHz stereo uncoupled, roughly 128kbps VBR) */
  vorbis_info_init (&vorbisenc->vi);

  if (vorbisenc->quality_set) {
    if (vorbis_encode_setup_vbr (&vorbisenc->vi,
            vorbisenc->channels, vorbisenc->frequency,
            vorbisenc->quality) != 0) {
      GST_ERROR_OBJECT (vorbisenc,
          "vorbisenc: initialisation failed: invalid parameters for quality");
      vorbis_info_clear (&vorbisenc->vi);
      return FALSE;
    }

    /* do we have optional hard quality restrictions? */
    if (vorbisenc->max_bitrate > 0 || vorbisenc->min_bitrate > 0) {
      struct ovectl_ratemanage_arg ai;

      vorbis_encode_ctl (&vorbisenc->vi, OV_ECTL_RATEMANAGE_GET, &ai);

      ai.bitrate_hard_min = vorbisenc->min_bitrate;
      ai.bitrate_hard_max = vorbisenc->max_bitrate;
      ai.management_active = 1;

      vorbis_encode_ctl (&vorbisenc->vi, OV_ECTL_RATEMANAGE_SET, &ai);
    }
  } else {
    long min_bitrate, max_bitrate;

    min_bitrate = vorbisenc->min_bitrate > 0 ? vorbisenc->min_bitrate : -1;
    max_bitrate = vorbisenc->max_bitrate > 0 ? vorbisenc->max_bitrate : -1;

    if (vorbis_encode_setup_managed (&vorbisenc->vi,
            vorbisenc->channels,
            vorbisenc->frequency,
            max_bitrate, vorbisenc->bitrate, min_bitrate) != 0) {
      GST_ERROR_OBJECT (vorbisenc,
          "vorbis_encode_setup_managed "
          "(c %d, rate %d, max br %ld, br %ld, min br %ld) failed",
          vorbisenc->channels, vorbisenc->frequency, max_bitrate,
          vorbisenc->bitrate, min_bitrate);
      vorbis_info_clear (&vorbisenc->vi);
      return FALSE;
    }
  }

  if (vorbisenc->managed && vorbisenc->bitrate < 0) {
    vorbis_encode_ctl (&vorbisenc->vi, OV_ECTL_RATEMANAGE_AVG, NULL);
  } else if (!vorbisenc->managed) {
    /* Turn off management entirely (if it was turned on). */
    vorbis_encode_ctl (&vorbisenc->vi, OV_ECTL_RATEMANAGE_SET, NULL);
  }
  vorbis_encode_setup_init (&vorbisenc->vi);

  /* set up the analysis state and auxiliary encoding storage */
  vorbis_analysis_init (&vorbisenc->vd, &vorbisenc->vi);
  vorbis_block_init (&vorbisenc->vd, &vorbisenc->vb);

  vorbisenc->next_ts = 0;

  vorbisenc->setup = TRUE;

  return TRUE;
}

static GstFlowReturn
gst_vorbisenc_clear (GstVorbisEnc * vorbisenc)
{
  GstFlowReturn ret = GST_FLOW_OK;

  if (vorbisenc->setup) {
    vorbis_analysis_wrote (&vorbisenc->vd, 0);
    ret = gst_vorbisenc_output_buffers (vorbisenc);

    vorbisenc->setup = FALSE;
  }

  /* clean up and exit.  vorbis_info_clear() must be called last */
  vorbis_block_clear (&vorbisenc->vb);
  vorbis_dsp_clear (&vorbisenc->vd);
  vorbis_info_clear (&vorbisenc->vi);

  vorbisenc->header_sent = FALSE;

  return ret;
}

/* prepare a buffer for transmission by passing data through libvorbis */
static GstBuffer *
gst_vorbisenc_buffer_from_packet (GstVorbisEnc * vorbisenc, ogg_packet * packet)
{
  GstBuffer *outbuf;

  outbuf = gst_buffer_new_and_alloc (packet->bytes);
  memcpy (GST_BUFFER_DATA (outbuf), packet->packet, packet->bytes);
  /* see ext/ogg/README; OFFSET_END takes "our" granulepos, OFFSET its
   * time representation */
  GST_BUFFER_OFFSET_END (outbuf) = packet->granulepos +
      vorbisenc->granulepos_offset;
  GST_BUFFER_OFFSET (outbuf) = granulepos_to_timestamp (vorbisenc,
      GST_BUFFER_OFFSET_END (outbuf));
  GST_BUFFER_TIMESTAMP (outbuf) = vorbisenc->next_ts;

  /* update the next timestamp, taking granulepos_offset and subgranule offset
   * into account */
  vorbisenc->next_ts =
      granulepos_to_timestamp_offset (vorbisenc, packet->granulepos);
  GST_BUFFER_DURATION (outbuf) =
      vorbisenc->next_ts - GST_BUFFER_TIMESTAMP (outbuf);

  GST_LOG_OBJECT (vorbisenc, "encoded buffer of %d bytes",
      GST_BUFFER_SIZE (outbuf));
  return outbuf;
}

/* the same as above, but different logic for setting timestamp and granulepos
 * */
static GstBuffer *
gst_vorbisenc_buffer_from_header_packet (GstVorbisEnc * vorbisenc,
    ogg_packet * packet)
{
  GstBuffer *outbuf;

  outbuf = gst_buffer_new_and_alloc (packet->bytes);
  memcpy (GST_BUFFER_DATA (outbuf), packet->packet, packet->bytes);
  GST_BUFFER_OFFSET (outbuf) = vorbisenc->bytes_out;
  GST_BUFFER_OFFSET_END (outbuf) = 0;
  GST_BUFFER_TIMESTAMP (outbuf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION (outbuf) = GST_CLOCK_TIME_NONE;

  GST_DEBUG ("created header packet buffer, %d bytes",
      GST_BUFFER_SIZE (outbuf));
  return outbuf;
}

/* push out the buffer and do internal bookkeeping */
static GstFlowReturn
gst_vorbisenc_push_buffer (GstVorbisEnc * vorbisenc, GstBuffer * buffer)
{
  vorbisenc->bytes_out += GST_BUFFER_SIZE (buffer);

  return gst_pad_push (vorbisenc->srcpad, buffer);
}

static GstFlowReturn
gst_vorbisenc_push_packet (GstVorbisEnc * vorbisenc, ogg_packet * packet)
{
  GstBuffer *outbuf;

  outbuf = gst_vorbisenc_buffer_from_packet (vorbisenc, packet);
  return gst_vorbisenc_push_buffer (vorbisenc, outbuf);
}

static GstCaps *
gst_vorbisenc_set_header_on_caps (GstCaps * caps, GstBuffer * buf1,
    GstBuffer * buf2, GstBuffer * buf3)
{
  GstStructure *structure;
  GValue array = { 0 };
  GValue value = { 0 };

  caps = gst_caps_make_writable (caps);
  structure = gst_caps_get_structure (caps, 0);

  /* mark buffers */
  GST_BUFFER_FLAG_SET (buf1, GST_BUFFER_FLAG_IN_CAPS);
  GST_BUFFER_FLAG_SET (buf2, GST_BUFFER_FLAG_IN_CAPS);
  GST_BUFFER_FLAG_SET (buf3, GST_BUFFER_FLAG_IN_CAPS);

  /* put buffers in a fixed list */
  g_value_init (&array, GST_TYPE_ARRAY);
  g_value_init (&value, GST_TYPE_BUFFER);
  gst_value_set_buffer (&value, buf1);
  gst_value_array_append_value (&array, &value);
  g_value_unset (&value);
  g_value_init (&value, GST_TYPE_BUFFER);
  gst_value_set_buffer (&value, buf2);
  gst_value_array_append_value (&array, &value);
  g_value_unset (&value);
  g_value_init (&value, GST_TYPE_BUFFER);
  gst_value_set_buffer (&value, buf3);
  gst_value_array_append_value (&array, &value);
  gst_structure_set_value (structure, "streamheader", &array);
  g_value_unset (&value);
  g_value_unset (&array);

  return caps;
}

static gboolean
gst_vorbisenc_sink_event (GstPad * pad, GstEvent * event)
{
  gboolean res = TRUE;
  GstVorbisEnc *vorbisenc;

  vorbisenc = GST_VORBISENC (GST_PAD_PARENT (pad));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      /* Tell the library we're at end of stream so that it can handle
       * the last frame and mark end of stream in the output properly */
      GST_DEBUG_OBJECT (vorbisenc, "EOS, clearing state and sending event on");
      gst_vorbisenc_clear (vorbisenc);

      res = gst_pad_push_event (vorbisenc->srcpad, event);
      break;
    case GST_EVENT_TAG:
      if (vorbisenc->tags) {
        GstTagList *list;

        gst_event_parse_tag (event, &list);
        gst_tag_list_insert (vorbisenc->tags, list,
            gst_tag_setter_get_tag_merge_mode (GST_TAG_SETTER (vorbisenc)));
      } else {
        g_assert_not_reached ();
      }
      res = gst_pad_push_event (vorbisenc->srcpad, event);
      break;
    default:
      res = gst_pad_push_event (vorbisenc->srcpad, event);
      break;
  }
  return res;
}

static GstFlowReturn
gst_vorbisenc_chain (GstPad * pad, GstBuffer * buffer)
{
  GstVorbisEnc *vorbisenc;
  GstFlowReturn ret = GST_FLOW_OK;
  gfloat *data;
  gulong size;
  gulong i, j;
  float **vorbis_buffer;

  vorbisenc = GST_VORBISENC (GST_PAD_PARENT (pad));

  if (!vorbisenc->setup)
    goto not_setup;

  if (!vorbisenc->header_sent) {
    /* Vorbis streams begin with three headers; the initial header (with
       most of the codec setup parameters) which is mandated by the Ogg
       bitstream spec.  The second header holds any comment fields.  The
       third header holds the bitstream codebook.  We merely need to
       make the headers, then pass them to libvorbis one at a time;
       libvorbis handles the additional Ogg bitstream constraints */
    ogg_packet header;
    ogg_packet header_comm;
    ogg_packet header_code;
    GstBuffer *buf1, *buf2, *buf3;
    GstCaps *caps;

    /* first, make sure header buffers get timestamp == 0 */
    vorbisenc->next_ts = 0;
    vorbisenc->granulepos_offset = 0;
    vorbisenc->subgranule_offset = 0;

    GST_DEBUG_OBJECT (vorbisenc, "creating and sending header packets");
    gst_vorbisenc_set_metadata (vorbisenc);
    vorbis_analysis_headerout (&vorbisenc->vd, &vorbisenc->vc, &header,
        &header_comm, &header_code);
    vorbis_comment_clear (&vorbisenc->vc);

    /* create header buffers */
    buf1 = gst_vorbisenc_buffer_from_header_packet (vorbisenc, &header);
    buf2 = gst_vorbisenc_buffer_from_header_packet (vorbisenc, &header_comm);
    buf3 = gst_vorbisenc_buffer_from_header_packet (vorbisenc, &header_code);

    /* mark and put on caps */
    caps = gst_pad_get_caps (vorbisenc->srcpad);
    caps = gst_vorbisenc_set_header_on_caps (caps, buf1, buf2, buf3);

    /* negotiate with these caps */
    GST_DEBUG ("here are the caps: %" GST_PTR_FORMAT, caps);
    gst_pad_set_caps (vorbisenc->srcpad, caps);

    gst_buffer_set_caps (buf1, caps);
    gst_buffer_set_caps (buf2, caps);
    gst_buffer_set_caps (buf3, caps);

    /* push out buffers */
    if ((ret = gst_vorbisenc_push_buffer (vorbisenc, buf1)) != GST_FLOW_OK)
      goto failed_header_push;
    if ((ret = gst_vorbisenc_push_buffer (vorbisenc, buf2)) != GST_FLOW_OK)
      goto failed_header_push;
    if ((ret = gst_vorbisenc_push_buffer (vorbisenc, buf3)) != GST_FLOW_OK)
      goto failed_header_push;


    /* now adjust starting granulepos accordingly if the buffer's timestamp is
       nonzero */
    vorbisenc->next_ts = GST_BUFFER_TIMESTAMP (buffer);
    vorbisenc->granulepos_offset = gst_util_uint64_scale
        (GST_BUFFER_TIMESTAMP (buffer), vorbisenc->frequency, GST_SECOND);
    vorbisenc->subgranule_offset = 0;
    vorbisenc->subgranule_offset =
        vorbisenc->next_ts - granulepos_to_timestamp_offset (vorbisenc, 0);

    vorbisenc->header_sent = TRUE;
  }

  /* data to encode */
  data = (gfloat *) GST_BUFFER_DATA (buffer);
  size = GST_BUFFER_SIZE (buffer) / (vorbisenc->channels * sizeof (float));

  /* expose the buffer to submit data */
  vorbis_buffer = vorbis_analysis_buffer (&vorbisenc->vd, size);

  /* deinterleave samples, write the buffer data */
  for (i = 0; i < size; i++) {
    for (j = 0; j < vorbisenc->channels; j++) {
      vorbis_buffer[j][i] = *data++;
    }
  }

  /* tell the library how much we actually submitted */
  vorbis_analysis_wrote (&vorbisenc->vd, size);

  vorbisenc->samples_in += size;

  gst_buffer_unref (buffer);

  ret = gst_vorbisenc_output_buffers (vorbisenc);

  return ret;

  /* error cases */
not_setup:
  {
    gst_buffer_unref (buffer);
    GST_ELEMENT_ERROR (vorbisenc, CORE, NEGOTIATION, (NULL),
        ("encoder not initialized (input is not audio?)"));
    return GST_FLOW_UNEXPECTED;
  }
failed_header_push:
  {
    gst_buffer_unref (buffer);
    return ret;
  }
}

static GstFlowReturn
gst_vorbisenc_output_buffers (GstVorbisEnc * vorbisenc)
{
  GstFlowReturn ret;

  /* vorbis does some data preanalysis, then divides up blocks for
     more involved (potentially parallel) processing.  Get a single
     block for encoding now */
  while (vorbis_analysis_blockout (&vorbisenc->vd, &vorbisenc->vb) == 1) {
    ogg_packet op;

    GST_LOG_OBJECT (vorbisenc, "analysed to a block");

    /* analysis */
    vorbis_analysis (&vorbisenc->vb, NULL);
    vorbis_bitrate_addblock (&vorbisenc->vb);

    while (vorbis_bitrate_flushpacket (&vorbisenc->vd, &op)) {
      GST_LOG_OBJECT (vorbisenc, "pushing out a data packet");
      ret = gst_vorbisenc_push_packet (vorbisenc, &op);

      if (ret != GST_FLOW_OK)
        return ret;
    }
  }

  return GST_FLOW_OK;
}

static void
gst_vorbisenc_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstVorbisEnc *vorbisenc;

  g_return_if_fail (GST_IS_VORBISENC (object));

  vorbisenc = GST_VORBISENC (object);

  switch (prop_id) {
    case ARG_MAX_BITRATE:
      g_value_set_int (value, vorbisenc->max_bitrate);
      break;
    case ARG_BITRATE:
      g_value_set_int (value, vorbisenc->bitrate);
      break;
    case ARG_MIN_BITRATE:
      g_value_set_int (value, vorbisenc->min_bitrate);
      break;
    case ARG_QUALITY:
      g_value_set_float (value, vorbisenc->quality);
      break;
    case ARG_MANAGED:
      g_value_set_boolean (value, vorbisenc->managed);
      break;
    case ARG_LAST_MESSAGE:
      g_value_set_string (value, vorbisenc->last_message);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vorbisenc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVorbisEnc *vorbisenc;

  g_return_if_fail (GST_IS_VORBISENC (object));

  vorbisenc = GST_VORBISENC (object);

  switch (prop_id) {
    case ARG_MAX_BITRATE:
    {
      gboolean old_value = vorbisenc->managed;

      vorbisenc->max_bitrate = g_value_get_int (value);
      if (vorbisenc->max_bitrate >= 0
          && vorbisenc->max_bitrate < LOWEST_BITRATE) {
        g_warning ("Lowest allowed bitrate is %d", LOWEST_BITRATE);
        vorbisenc->max_bitrate = LOWEST_BITRATE;
      }
      if (vorbisenc->min_bitrate > 0 && vorbisenc->max_bitrate > 0)
        vorbisenc->managed = TRUE;
      else
        vorbisenc->managed = FALSE;

      if (old_value != vorbisenc->managed)
        g_object_notify (object, "managed");
      break;
    }
    case ARG_BITRATE:
      vorbisenc->bitrate = g_value_get_int (value);
      if (vorbisenc->bitrate >= 0 && vorbisenc->bitrate < LOWEST_BITRATE) {
        g_warning ("Lowest allowed bitrate is %d", LOWEST_BITRATE);
        vorbisenc->bitrate = LOWEST_BITRATE;
      }
      break;
    case ARG_MIN_BITRATE:
    {
      gboolean old_value = vorbisenc->managed;

      vorbisenc->min_bitrate = g_value_get_int (value);
      if (vorbisenc->min_bitrate >= 0
          && vorbisenc->min_bitrate < LOWEST_BITRATE) {
        g_warning ("Lowest allowed bitrate is %d", LOWEST_BITRATE);
        vorbisenc->min_bitrate = LOWEST_BITRATE;
      }
      if (vorbisenc->min_bitrate > 0 && vorbisenc->max_bitrate > 0)
        vorbisenc->managed = TRUE;
      else
        vorbisenc->managed = FALSE;

      if (old_value != vorbisenc->managed)
        g_object_notify (object, "managed");
      break;
    }
    case ARG_QUALITY:
      vorbisenc->quality = g_value_get_float (value);
      if (vorbisenc->quality >= 0.0)
        vorbisenc->quality_set = TRUE;
      else
        vorbisenc->quality_set = FALSE;
      break;
    case ARG_MANAGED:
      vorbisenc->managed = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_vorbisenc_change_state (GstElement * element, GstStateChange transition)
{
  GstVorbisEnc *vorbisenc = GST_VORBISENC (element);
  GstStateChangeReturn res;


  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      vorbisenc->tags = gst_tag_list_new ();
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      vorbisenc->setup = FALSE;
      vorbisenc->header_sent = FALSE;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    default:
      break;
  }

  res = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      vorbis_block_clear (&vorbisenc->vb);
      vorbis_dsp_clear (&vorbisenc->vd);
      vorbis_info_clear (&vorbisenc->vi);
      g_free (vorbisenc->last_message);
      vorbisenc->last_message = NULL;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_tag_list_free (vorbisenc->tags);
      vorbisenc->tags = NULL;
    default:
      break;
  }

  return res;
}
