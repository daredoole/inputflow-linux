package com.inputflow.android

import org.json.JSONObject
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.Socket
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

object RelayProtocol {
    private const val MAX_FRAME_BYTES = 64 * 1024

    fun readFrame(input: DataInputStream): JSONObject {
        val length = input.readInt()
        require(length in 1..MAX_FRAME_BYTES) { "Invalid frame length $length" }
        val bytes = ByteArray(length)
        input.readFully(bytes)
        return JSONObject(bytes.toString(Charsets.UTF_8))
    }

    fun writeFrame(output: DataOutputStream, json: JSONObject) {
        val bytes = json.toString().toByteArray(Charsets.UTF_8)
        require(bytes.size in 1..MAX_FRAME_BYTES) { "Invalid frame length ${bytes.size}" }
        output.writeInt(bytes.size)
        output.write(bytes)
        output.flush()
    }

    fun authenticate(socket: Socket, secret: String, deviceName: String): Pair<DataInputStream, DataOutputStream> {
        val input = DataInputStream(socket.getInputStream().buffered())
        val output = DataOutputStream(socket.getOutputStream().buffered())
        val hello = readFrame(input)
        require(hello.optString("type") == "hello") { "Expected hello frame" }
        val nonce = hello.getString("nonce")
        val auth = JSONObject()
            .put("type", "auth")
            .put("device", deviceName)
            .put("hmac", hmacSha256Hex(secret, nonce))
        writeFrame(output, auth)
        val ready = readFrame(input)
        require(ready.optString("type") == "ready") { "Expected ready frame" }
        return input to output
    }

    private fun hmacSha256Hex(secret: String, message: String): String {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(secret.toByteArray(Charsets.UTF_8), "HmacSHA256"))
        return mac.doFinal(message.toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it) }
    }
}
