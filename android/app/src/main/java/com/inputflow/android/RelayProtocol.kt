package com.inputflow.android

import org.json.JSONObject
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.Socket
import java.nio.ByteBuffer
import java.util.Arrays
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

object RelayProtocol {
    private const val MAX_FRAME_BYTES = 64 * 1024
    private const val ENCRYPTED_FRAME_OVERHEAD = 4 + 8 + 16
    private const val MAX_WIRE_FRAME_BYTES = MAX_FRAME_BYTES + ENCRYPTED_FRAME_OVERHEAD
    private val FRAME_MAGIC = byteArrayOf('I'.code.toByte(), 'F'.code.toByte(), 'P'.code.toByte(), '1'.code.toByte())
    internal val SERVER_TO_CLIENT_IV = byteArrayOf('I'.code.toByte(), 'F'.code.toByte(), 'S'.code.toByte(), 'O'.code.toByte())
    internal val CLIENT_TO_SERVER_IV = byteArrayOf('I'.code.toByte(), 'F'.code.toByte(), 'C'.code.toByte(), 'O'.code.toByte())
    private val NONCE_PATTERN = Regex("[0-9a-f]{32}")

    class Session internal constructor(
        private val input: DataInputStream,
        private val output: DataOutputStream,
        private val readKey: ByteArray,
        private val writeKey: ByteArray,
    ) {
        private var readSequence = 0L
        private var writeSequence = 0L

        fun readFrame(): JSONObject {
            require(readSequence != Long.MAX_VALUE) { "Relay receive sequence exhausted" }
            val envelope = readWireFrame(input, MAX_WIRE_FRAME_BYTES)
            val plaintext = decryptPayload(
                envelope,
                readKey,
                readSequence,
                SERVER_TO_CLIENT_IV,
            )
            readSequence += 1
            return JSONObject(plaintext.toString(Charsets.UTF_8))
        }

        @Synchronized
        fun writeFrame(json: JSONObject) {
            require(writeSequence != Long.MAX_VALUE) { "Relay send sequence exhausted" }
            val plaintext = json.toString().toByteArray(Charsets.UTF_8)
            require(plaintext.size in 1..MAX_FRAME_BYTES) {
                "Invalid frame length ${plaintext.size}"
            }
            val envelope = encryptPayload(
                plaintext,
                writeKey,
                writeSequence,
                CLIENT_TO_SERVER_IV,
            )
            writeSequence += 1
            writeWireFrame(output, envelope)
        }

        fun hasBufferedFrame(): Boolean = input.available() > Int.SIZE_BYTES

        fun destroy() {
            Arrays.fill(readKey, 0)
            Arrays.fill(writeKey, 0)
        }
    }

    fun authenticate(socket: Socket, secret: String, deviceName: String): Session {
        val input = DataInputStream(socket.getInputStream().buffered())
        val output = DataOutputStream(socket.getOutputStream().buffered())
        val hello = readPlainFrame(input)
        require(hello.optString("type") == "hello") { "Expected hello frame" }
        require(hello.optInt("version") == 2) { "Unsupported relay protocol" }
        require(hello.optString("cipher") == "AES-256-GCM") { "Unsupported relay cipher" }
        val nonce = hello.getString("nonce")
        require(NONCE_PATTERN.matches(nonce)) { "Invalid relay nonce" }
        val auth = JSONObject()
            .put("type", "auth")
            .put("device", deviceName)
            .put("hmac", hmacSha256Hex(secret, nonce))
        writePlainFrame(output, auth)

        val session = Session(
            input,
            output,
            deriveSessionKey(secret, nonce, "server-to-client"),
            deriveSessionKey(secret, nonce, "client-to-server"),
        )
        val ready = session.readFrame()
        require(ready.optString("type") == "ready") { "Expected ready frame" }
        return session
    }

    private fun readPlainFrame(input: DataInputStream): JSONObject =
        JSONObject(readWireFrame(input, MAX_FRAME_BYTES).toString(Charsets.UTF_8))

    private fun writePlainFrame(output: DataOutputStream, json: JSONObject) {
        val bytes = json.toString().toByteArray(Charsets.UTF_8)
        require(bytes.size in 1..MAX_FRAME_BYTES) { "Invalid frame length ${bytes.size}" }
        writeWireFrame(output, bytes)
    }

    private fun readWireFrame(input: DataInputStream, maximumLength: Int): ByteArray {
        val length = input.readInt()
        require(length in 1..maximumLength) { "Invalid relay frame length" }
        return ByteArray(length).also(input::readFully)
    }

    private fun writeWireFrame(output: DataOutputStream, bytes: ByteArray) {
        output.writeInt(bytes.size)
        output.write(bytes)
        output.flush()
    }

    internal fun deriveSessionKey(secret: String, nonce: String, direction: String): ByteArray =
        hmacSha256(secret, "inputflow-relay-v1/$direction/$nonce")

    internal fun encryptPayload(
        plaintext: ByteArray,
        key: ByteArray,
        sequence: Long,
        ivPrefix: ByteArray,
    ): ByteArray {
        val header = ByteBuffer.allocate(12)
            .put(FRAME_MAGIC)
            .putLong(sequence)
            .array()
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(
            Cipher.ENCRYPT_MODE,
            SecretKeySpec(key, "AES"),
            GCMParameterSpec(128, buildIv(ivPrefix, sequence)),
        )
        cipher.updateAAD(header)
        return header + cipher.doFinal(plaintext)
    }

    internal fun decryptPayload(
        envelope: ByteArray,
        key: ByteArray,
        expectedSequence: Long,
        ivPrefix: ByteArray,
    ): ByteArray {
        require(envelope.size in (ENCRYPTED_FRAME_OVERHEAD + 1)..MAX_WIRE_FRAME_BYTES) {
            "Invalid encrypted relay frame"
        }
        require(envelope.copyOfRange(0, FRAME_MAGIC.size).contentEquals(FRAME_MAGIC)) {
            "Invalid encrypted relay frame"
        }
        val header = envelope.copyOfRange(0, 12)
        val sequence = ByteBuffer.wrap(header, FRAME_MAGIC.size, Long.SIZE_BYTES).long
        require(sequence == expectedSequence) { "Invalid relay sequence" }

        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(
            Cipher.DECRYPT_MODE,
            SecretKeySpec(key, "AES"),
            GCMParameterSpec(128, buildIv(ivPrefix, sequence)),
        )
        cipher.updateAAD(header)
        return cipher.doFinal(envelope, header.size, envelope.size - header.size)
    }

    private fun buildIv(prefix: ByteArray, sequence: Long): ByteArray =
        ByteBuffer.allocate(12).put(prefix).putLong(sequence).array()

    private fun hmacSha256(secret: String, message: String): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(secret.toByteArray(Charsets.UTF_8), "HmacSHA256"))
        return mac.doFinal(message.toByteArray(Charsets.UTF_8))
    }

    private fun hmacSha256Hex(secret: String, message: String): String {
        val digits = "0123456789abcdef"
        return buildString(64) {
            for (byte in hmacSha256(secret, message)) {
                val value = byte.toInt() and 0xff
                append(digits[value ushr 4])
                append(digits[value and 0x0f])
            }
        }
    }
}
