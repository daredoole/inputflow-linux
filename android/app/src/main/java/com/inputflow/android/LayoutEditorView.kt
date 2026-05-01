package com.inputflow.android

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import kotlin.math.roundToInt

data class DeviceRect(
    val id: String,
    val name: String,
    var gridX: Int,
    var gridY: Int,
    val gridW: Int,
    val gridH: Int,
    val isLocal: Boolean
)

class LayoutEditorView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    var devices: List<DeviceRect> = emptyList()
        set(value) {
            field = value
            centerDevices()
            invalidate()
        }

    private val gridCellPx get() = (64 * resources.displayMetrics.density).toInt()

    private val bgPaint = Paint().apply {
        color = Color.parseColor("#0A1628")
        style = Paint.Style.FILL
    }
    private val gridPaint = Paint().apply {
        color = Color.parseColor("#1A2840")
        style = Paint.Style.STROKE
        strokeWidth = 1f
    }
    private val deviceFillPaint = Paint().apply {
        style = Paint.Style.FILL
        isAntiAlias = true
    }
    private val deviceStrokePaint = Paint().apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        isAntiAlias = true
    }
    private val labelPaint = Paint().apply {
        color = Color.WHITE
        textSize = 14f * resources.displayMetrics.density
        isAntiAlias = true
        typeface = Typeface.DEFAULT_BOLD
        textAlign = Paint.Align.CENTER
    }
    private val badgePaint = Paint().apply {
        color = Color.parseColor("#1A2840")
        textSize = 10f * resources.displayMetrics.density
        isAntiAlias = true
        textAlign = Paint.Align.CENTER
    }

    private var draggingId: String? = null
    private var touchOffsetX = 0f
    private var touchOffsetY = 0f
    private var viewOffsetX = 0
    private var viewOffsetY = 0

    private fun centerDevices() {
        if (devices.isEmpty() || width == 0 || height == 0) return
        val minGx = devices.minOf { it.gridX }
        val minGy = devices.minOf { it.gridY }
        val maxGx = devices.maxOf { it.gridX + it.gridW }
        val maxGy = devices.maxOf { it.gridY + it.gridH }
        val totalW = (maxGx - minGx) * gridCellPx
        val totalH = (maxGy - minGy) * gridCellPx
        viewOffsetX = (width - totalW) / 2 - minGx * gridCellPx
        viewOffsetY = (height - totalH) / 2 - minGy * gridCellPx
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        centerDevices()
    }

    override fun onDraw(canvas: Canvas) {
        // Background
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), bgPaint)

        // Grid
        val cell = gridCellPx.toFloat()
        var gx = (viewOffsetX % cell + cell) % cell
        while (gx < width) {
            canvas.drawLine(gx, 0f, gx, height.toFloat(), gridPaint)
            gx += cell
        }
        var gy = (viewOffsetY % cell + cell) % cell
        while (gy < height) {
            canvas.drawLine(0f, gy, width.toFloat(), gy, gridPaint)
            gy += cell
        }

        // Devices
        for (device in devices) {
            val px = device.gridX * gridCellPx + viewOffsetX
            val py = device.gridY * gridCellPx + viewOffsetY
            val pw = device.gridW * gridCellPx
            val ph = device.gridH * gridCellPx
            val rect = RectF(px.toFloat(), py.toFloat(), (px + pw).toFloat(), (py + ph).toFloat())
            val radius = 12f * resources.displayMetrics.density

            deviceFillPaint.color = if (device.isLocal)
                Color.parseColor("#1B5E20")
            else
                Color.parseColor("#0D47A1")
            deviceFillPaint.alpha = 200
            canvas.drawRoundRect(rect, radius, radius, deviceFillPaint)

            deviceStrokePaint.color = if (device.isLocal)
                Color.parseColor("#66BB6A")
            else
                Color.parseColor("#42A5F5")
            canvas.drawRoundRect(rect, radius, radius, deviceStrokePaint)

            // Label
            val cx = rect.centerX()
            val cy = rect.centerY()
            val lineH = labelPaint.textSize
            if (device.isLocal) {
                canvas.drawText(device.name, cx, cy - lineH * 0.2f, labelPaint)
                canvas.drawText(context.getString(R.string.editor_this_device), cx, cy + lineH * 0.9f, badgePaint)
            } else {
                canvas.drawText(device.name, cx, cy + lineH * 0.35f, labelPaint)
            }
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val cell = gridCellPx
                for (device in devices.reversed()) {
                    if (device.isLocal) continue
                    val px = device.gridX * cell + viewOffsetX
                    val py = device.gridY * cell + viewOffsetY
                    val pw = device.gridW * cell
                    val ph = device.gridH * cell
                    if (event.x >= px && event.x <= px + pw && event.y >= py && event.y <= py + ph) {
                        draggingId = device.id
                        touchOffsetX = event.x - px
                        touchOffsetY = event.y - py
                        return true
                    }
                }
            }
            MotionEvent.ACTION_MOVE -> {
                val id = draggingId ?: return false
                val device = devices.find { it.id == id } ?: return false
                val cell = gridCellPx
                val newPx = event.x - touchOffsetX - viewOffsetX
                val newPy = event.y - touchOffsetY - viewOffsetY
                device.gridX = (newPx / cell).roundToInt()
                device.gridY = (newPy / cell).roundToInt()
                invalidate()
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                draggingId = null
                invalidate()
            }
        }
        return super.onTouchEvent(event)
    }

    fun getLayoutJson(): org.json.JSONArray {
        val arr = org.json.JSONArray()
        for (device in devices) {
            arr.put(
                org.json.JSONObject()
                    .put("id", device.id)
                    .put("x", device.gridX * gridCellPx)
                    .put("y", device.gridY * gridCellPx)
            )
        }
        return arr
    }
}
