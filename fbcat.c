/* Copyright © 2009 Piotr Lewandowski, Jakub Wilk
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 dated June, 1991.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/fb.h>

#if !defined(le32toh) || !defined(le16toh)

#if BYTE_ORDER == LITTLE_ENDIAN
#define le32toh(x) (x)
#define le16toh(x) (x)
#else
#include <byteswap.h>
#define le32toh(x) bswap_32(x)
#define le16toh(x) bswap_16(x)
#endif

#endif

#define DEFAULT_FBDEV "/dev/fb0"

static void posix_error(const char *s, ...)
{
  va_list argv;
  va_start(argv, s);
  vfprintf(stderr, s, argv);
  fprintf(stderr, ": ");
  perror(NULL);
  va_end(argv);
  exit(2);
}

static void not_supported(const char *s)
{
  fprintf(stderr,
    "Framebuffer device is not supported: %s\n"
    "Please file a bug at http://code.google.com/p/fbcat/issues/\n",
    s);
  exit(3);
}

static inline unsigned char get_color(unsigned int pixel, const struct fb_bitfield *bitfield, uint16_t *colormap)
{
  return colormap[(pixel >> bitfield->offset) & ((1 << bitfield->length) - 1)] >> 8;
}

static void dump_video_memory(
  const unsigned char *video_memory,
  const struct fb_var_screeninfo *info,
  const struct fb_cmap *colormap, 
  size_t line_length,
  FILE *fp
)
{
  unsigned int x, y;
  const size_t bytes_per_pixel = (info->bits_per_pixel + 7) / 8;
  unsigned char *row = malloc(info->xres * 3);
  if (row == NULL)
    posix_error("malloc failed");

  fprintf(fp, "P6 %" PRIu32 " %" PRIu32 " 255\n", info->xres, info->yres);
  for (y = 0; y < info->yres; y++)
  {
    const unsigned char *current;
    current = video_memory + (y + info->yoffset) * line_length + info->xoffset * bytes_per_pixel;
    for (x = 0; x < info->xres; x++)
    {
      unsigned int i;
      unsigned int pixel = 0;
      switch (bytes_per_pixel)
      {
        /* The following code assumes little-endian byte ordering in the frame
         * buffer. */
        case 4:
          pixel = le32toh(*((uint32_t *) current));
          current += 4;
          break;
        case 2:
          pixel = le16toh(*((uint16_t *) current));
          current += 2;
          break;
        default:
          for (i = 0; i < bytes_per_pixel; i++)
          {
            pixel |= current[0] << (i * 8);
            current++;
          }
          break;
      }
      row[x * 3 + 0] = get_color(pixel, &info->red, colormap->red);
      row[x * 3 + 1] = get_color(pixel, &info->green, colormap->green);
      row[x * 3 + 2] = get_color(pixel, &info->blue, colormap->blue);
    }
    if (fwrite(row, 1, info->xres * 3, fp) != info->xres * 3)
      posix_error("Write error");
  }

  free(row);
}

int main(int argc, const char **argv)
{
  const char *fbdev_name;
  int fd;

  bool show_usage = false;
  if (isatty(STDOUT_FILENO))
  {
    fprintf(stderr, "I won't write binary data to a terminal.\n");
    show_usage = true;
  }
  if (argc > 2)
    show_usage = true;
  if (show_usage)
  {
    fprintf(stderr, "Usage: %s [fbdev]\n", argv[0]);
    return 1;
  }

  if (argc == 2)
    fbdev_name = argv[1];
  else
  {
    fbdev_name = getenv("FRAMEBUFFER");
    if (fbdev_name == NULL || fbdev_name[0] == '\0')
      fbdev_name = DEFAULT_FBDEV;
  }

  fd = open(fbdev_name, O_RDONLY);
  if (fd == -1)
    posix_error("Could not open %s", fbdev_name);

  struct fb_fix_screeninfo fix_info;
  struct fb_var_screeninfo var_info;
  uint16_t colormap_data[4][1 << 8];
  struct fb_cmap colormap =
  {
    0,
    1 << 8,
    colormap_data[0],
    colormap_data[1],
    colormap_data[2],
    colormap_data[3],
  };

  if (ioctl(fd, FBIOGET_FSCREENINFO, &fix_info))
    posix_error("FBIOGET_FSCREENINFO failed");

  if (fix_info.type != FB_TYPE_PACKED_PIXELS)
    not_supported("framebuffer type is not PACKED_PIXELS");

  if (ioctl(fd, FBIOGET_VSCREENINFO, &var_info))
    posix_error("FBIOGET_VSCREENINFO failed");

  if (var_info.red.length > 8 || var_info.green.length > 8 || var_info.blue.length > 8)
    not_supported("color depth > 8 bits per component");

  switch (fix_info.visual)
  {
    case FB_VISUAL_TRUECOLOR:
    {
      /* initialize dummy colormap */
      unsigned int i;
      for (i = 0; i < (1 << var_info.red.length); i++)
        colormap.red[i] = i * 0xffff / ((1 << var_info.red.length) - 1);
      for (i = 0; i < (1 << var_info.green.length); i++)
        colormap.green[i] = i * 0xffff / ((1 << var_info.green.length) - 1);
      for (i = 0; i < (1 << var_info.blue.length); i++)
        colormap.blue[i] = i * 0xffff / ((1 << var_info.blue.length) - 1);
      break;
    }
    case FB_VISUAL_DIRECTCOLOR:
    case FB_VISUAL_PSEUDOCOLOR:
    case FB_VISUAL_STATIC_PSEUDOCOLOR:
      if (ioctl(fd, FBIOGETCMAP, &colormap) != 0)
        posix_error("FBIOGETCMAP failed");
      break;
    default:
      not_supported("unsupported visual");
  }

  if (var_info.bits_per_pixel < 8)
    not_supported("< 8 bpp");
  const size_t mapped_length = fix_info.line_length * (var_info.yres + var_info.yoffset);
  unsigned char *video_memory = mmap(NULL, mapped_length, PROT_READ, MAP_SHARED, fd, 0);
  if (video_memory == MAP_FAILED)
    posix_error("mmap failed");

  dump_video_memory(video_memory, &var_info, &colormap, fix_info.line_length, stdout);

  /* deliberately ignore errors */
  munmap(video_memory, mapped_length);
  close(fd);
  return 0;
}

/* vim:set ts=2 sw=2 et: */
