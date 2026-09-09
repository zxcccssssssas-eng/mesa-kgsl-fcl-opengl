package com.mio.plugin.renderer

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.widget.Button
import android.widget.LinearLayout
import android.widget.Toast
import android.app.Activity
import android.os.Bundle
import android.util.TypedValue
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Loading this library runs the one-shot FCLProbe diagnostic in its
        // constructor (vendor EGL AHardwareBuffer import check, tag FCLProbe).
        runCatching { System.loadLibrary("freedreno_kgsl_init") }
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

                Presentation switch (FCL custom environment):
                FCL_SHIM_RENDERER=egl — default, AHB shared buffers.
                FCL_SHIM_RENDERER=vulkan — Vulkan presentation with
                synchronized CPU upload; slower, useful for comparison.

                Copy a setting below, add it in FCL's custom environment,
                then fully restart the game. This changes presentation only;
                the game still renders OpenGL through Freedreno/KGSL.
                If Vulkan cannot initialize, the shim logs an EGL fallback.

                GPU: Qualcomm Adreno (6xx / 7xx / 8xx) via KGSL.
                Mesa Gallium Freedreno over Android EGL (libEGL_mesa.so),
                not OSMesa (removed in Mesa 26) and not GL4ES / MobileGlues.

                No MESA_GL_VERSION_OVERRIDE is forced: Mesa reports the
                version the Adreno + KGSL stack really supports, which keeps
                NeoForge/GLFW early display from failing hard.

                If FCL still lists an old copy after updating this app,
                force-stop FCL so it rescans the plugin.
            """.trimIndent()
        }
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(text)
            for (backend in listOf("egl", "vulkan")) {
                addView(Button(this@MainActivity).apply {
                    this.text = "Copy ${backend.uppercase()} setting"
                    setOnClickListener {
                        val value = "FCL_SHIM_RENDERER=$backend"
                        (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
                            .setPrimaryClip(ClipData.newPlainText("FCL presentation", value))
                        Toast.makeText(this@MainActivity, "Copied. Paste into FCL custom environment.",
                            Toast.LENGTH_LONG).show()
                    }
                })
            }
        }
        setContentView(ScrollView(this).apply { addView(content) })
    }
}
