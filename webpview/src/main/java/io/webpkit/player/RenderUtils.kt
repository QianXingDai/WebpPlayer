package io.webpkit.player

import android.opengl.GLES20

object RenderUtils {

    fun createProgram(vs: String, fs: String): Int {
        val v = createShader(GLES20.GL_VERTEX_SHADER, vs)
        val f = createShader(GLES20.GL_FRAGMENT_SHADER, fs)
        val prog = GLES20.glCreateProgram()
        GLES20.glAttachShader(prog, v)
        GLES20.glAttachShader(prog, f)
        GLES20.glLinkProgram(prog)
        val linkStatus = IntArray(1)
        GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, linkStatus, 0)
        if (linkStatus[0] != GLES20.GL_TRUE) {
            val msg = GLES20.glGetProgramInfoLog(prog)
            GLES20.glDeleteProgram(prog)
            throw RuntimeException("Could not link program: $msg")
        }
        return prog
    }

    fun createShader(type: Int, src: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, src)
        GLES20.glCompileShader(shader)
        val compiled = IntArray(1)
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, compiled, 0)
        if (compiled[0] == 0) {
            val msg = GLES20.glGetShaderInfoLog(shader)
            WebpLog.e("RenderUtils", "Could not compile shader $type:\n$msg")
            GLES20.glDeleteShader(shader)
            throw RuntimeException("Shader compile failed")
        }
        return shader
    }
}