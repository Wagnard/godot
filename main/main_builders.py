"""Functions used to generate source files during build time"""

import methods


def make_splash(target, source, env):
    buffer = methods.get_buffer(str(source[0]))

    with methods.generated_wrapper(str(target[0])) as file:
        # The background of the Wagnardsoft splash (main/splash.png), whose borders are this colour (14, 14, 26), so that the
        # image blends into a window of any size.
        file.write(f"""\
#include "core/math/color.h"

static const Color boot_splash_bg_color = Color(0.055, 0.055, 0.1);
inline constexpr const unsigned char boot_splash_png[] = {{
	{methods.format_buffer(buffer, 1)}
}};
""")


def make_splash_editor(target, source, env):
    buffer = methods.get_buffer(str(source[0]))

    with methods.generated_wrapper(str(target[0])) as file:
        # The editor splash background color is taken from the default editor theme's background color.
        # This helps achieve a visually "smoother" transition between the splash screen and the editor.
        file.write(f"""\
#include "core/math/color.h"

static const Color boot_splash_editor_bg_color = Color(0.125, 0.145, 0.192);
inline constexpr const unsigned char boot_splash_editor_png[] = {{
	{methods.format_buffer(buffer, 1)}
}};
""")


def make_app_icon(target, source, env):
    buffer = methods.get_buffer(str(source[0]))

    with methods.generated_wrapper(str(target[0])) as file:
        # Use a neutral gray color to better fit various kinds of projects.
        file.write(f"""\
inline constexpr const unsigned char app_icon_png[] = {{
	{methods.format_buffer(buffer, 1)}
}};
""")
