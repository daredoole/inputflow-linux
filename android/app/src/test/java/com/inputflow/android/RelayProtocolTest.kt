package com.inputflow.android

import javax.crypto.AEADBadTagException
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class RelayProtocolTest {
    private val secret = "inputflow-test-only-not-a-real-credential"
    private val nonce = "00112233445566778899aabbccddeeff"

    @Test
    fun encryptedFrameRoundTripsWithoutPlaintextLeakage() {
        val key = RelayProtocol.deriveSessionKey(secret, nonce, "server-to-client")
        val plaintext = """{"type":"keyboard","vkCode":65,"flags":0}""".toByteArray()
        val encrypted = RelayProtocol.encryptPayload(
            plaintext,
            key,
            0,
            RelayProtocol.SERVER_TO_CLIENT_IV,
        )

        assertFalse(encrypted.toString(Charsets.UTF_8).contains("keyboard"))
        assertArrayEquals(
            plaintext,
            RelayProtocol.decryptPayload(
                encrypted,
                key,
                0,
                RelayProtocol.SERVER_TO_CLIENT_IV,
            ),
        )
    }

    @Test
    fun directionalKeysAreSeparated() {
        val serverKey = RelayProtocol.deriveSessionKey(secret, nonce, "server-to-client")
        val clientKey = RelayProtocol.deriveSessionKey(secret, nonce, "client-to-server")
        assertNotEquals(serverKey.toList(), clientKey.toList())
    }

    @Test
    fun tamperingAndReplaySequenceAreRejected() {
        val key = RelayProtocol.deriveSessionKey(secret, nonce, "server-to-client")
        val encrypted = RelayProtocol.encryptPayload(
            """{"type":"ready"}""".toByteArray(),
            key,
            7,
            RelayProtocol.SERVER_TO_CLIENT_IV,
        )
        encrypted[encrypted.lastIndex] = (encrypted.last().toInt() xor 1).toByte()

        assertThrows(AEADBadTagException::class.java) {
            RelayProtocol.decryptPayload(
                encrypted,
                key,
                7,
                RelayProtocol.SERVER_TO_CLIENT_IV,
            )
        }
        assertThrows(IllegalArgumentException::class.java) {
            RelayProtocol.decryptPayload(
                encrypted,
                key,
                8,
                RelayProtocol.SERVER_TO_CLIENT_IV,
            )
        }
    }
}
