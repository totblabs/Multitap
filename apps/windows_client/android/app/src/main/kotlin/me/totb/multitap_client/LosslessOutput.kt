package me.totb.multitap_client

/**
 * The picture size AndroidTileRenderer draws a start at ([width] x
 * [height]) and whether that is a larger source than the decoded video
 * ([scaled]: the Host downscaled the stream; its tiles describe the source).
 *
 * Unscaled, the output is the video's own size, as before. Scaled (a single
 * start only: the Host never sends scaled tiles for an output of tracks-v1),
 * the output is the source size announced at the start, the video upscaled
 * bilinearly under absent tiles, as the Windows client publishes a scaled
 * source. A scaled output costs the tile texture and the Flutter texture's
 * buffers at the source size, so it is admitted against the device's memory
 * reserve (AvcStreamSizes.memoryBudget) together with the decoder's own
 * estimate; the GPU limits are checked once a GL context exists ([fitsGpu]).
 */
internal data class LosslessOutput(val width: Int, val height: Int, val scaled: Boolean) {
    companion object {
        /** RGBA bytes per source pixel a scaled output adds: the tile texture and three texture buffers. */
        const val SCALED_BYTES_PER_PIXEL = 16L

        /**
         * The output for a start of [videoWidth] x [videoHeight] whose source is
         * [sourceWidth] x [sourceHeight] (null: the same), or null when this
         * start cannot draw tiles (plain video, no opt-in). [memoryBudget] is
         * asked only for a scaled source.
         */
        fun plan(videoWidth: Int, videoHeight: Int, sourceWidth: Int?, sourceHeight: Int?, allowScaled: Boolean,
            memoryBudget: () -> Long): LosslessOutput? {
            if (!LosslessTiles.validFrame(videoWidth, videoHeight)) return null
            val width = sourceWidth ?: videoWidth
            val height = sourceHeight ?: videoHeight
            if (width == videoWidth && height == videoHeight) return LosslessOutput(videoWidth, videoHeight, false)
            if (!allowScaled || width < videoWidth || height < videoHeight || !LosslessTiles.validFrame(width, height)) return null
            val decoder = streamSizeMemoryBytes(listOf(StreamSizeTrack(videoWidth, videoHeight, 1))) ?: return null
            val output = width.toLong() * height * SCALED_BYTES_PER_PIXEL
            val budget = memoryBudget()
            if (budget <= 0 || decoder + output > budget) return null
            return LosslessOutput(width, height, true)
        }

        /**
         * This GL context can draw [width] x [height] (the output) from a
         * [videoWidth] x [videoHeight] video: the tile texture and the video
         * within GL_MAX_TEXTURE_SIZE, the viewport within GL_MAX_VIEWPORT_DIMS.
         */
        fun fitsGpu(width: Int, height: Int, videoWidth: Int, videoHeight: Int, maxTextureSize: Int, maxViewportWidth: Int,
            maxViewportHeight: Int): Boolean =
            maxOf(width, height, videoWidth, videoHeight) <= maxTextureSize && width <= maxViewportWidth && height <= maxViewportHeight
    }
}
