package com.mio.plugin.renderer

import android.app.Activity
import android.os.Bundle
import android.util.TypedValue
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val text = TextView(this).apply {
            setPadding(pad, pad, pad, pad)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            this.text = """
                FCL Freedreno KGSL renderer plugin

                Package: $packageName
                Native dir: ${applicationInfo.nativeLibraryDir}

                FoldCraftLauncher discovers this APK automatically
                (meta-data fclPlugin=true). Restart FCL after install,
                then pick "Freedreno KGSL" as the renderer.

                GPU: Qualcomm Adreno (6xx / 7xx / 8xx) via KGSL.
                Mesa Gallium Freedreno over Android EGL (libEGL_mesa.so),
                not OSMesa (removed in Mesa 26) and not GL4ES / MobileGlues.

                OpenGL 4.6 is requested via MESA_GL_VERSION_OVERRIDE.
                The version string Minecraft shows still depends on
                the Adreno generation and Mesa feature support.
            """.trimIndent()
        }
        setContentView(ScrollView(this).apply { addView(text) })
    }
}
