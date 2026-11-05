package org.amnezia.vpn.protocol

import android.os.Bundle

private const val RX_BYTES_KEY = "rxBytes"
private const val TX_BYTES_KEY = "txBytes"
private const val LAST_HANDSHAKE_KEY = "lastHandshake"

@Suppress("DataClassPrivateConstructor")
data class Statistics private constructor(
    val rxBytes: Long = 0L,
    val txBytes: Long = 0L,
    val lastHandshake: Long = 0L
) {

    private constructor(builder: Builder) : this(builder.rxBytes, builder.txBytes, builder.lastHandshake)

    @Suppress("SuspiciousEqualsCombination")
    fun isEmpty(): Boolean = this === EMPTY_STATISTICS || this == EMPTY_STATISTICS

    class Builder {
        var rxBytes: Long = 0L
            private set

        var txBytes: Long = 0L
            private set

        var lastHandshake: Long = 0L
            private set

        fun setRxBytes(rxBytes: Long) = apply { this.rxBytes = rxBytes }
        fun setTxBytes(txBytes: Long) = apply { this.txBytes = txBytes }
        fun setLastHandshake(lastHandshake: Long) = apply { this.lastHandshake = lastHandshake }

        fun build(): Statistics =
            if (rxBytes + txBytes == 0L) EMPTY_STATISTICS
            else Statistics(this)
    }

    companion object {
        val EMPTY_STATISTICS: Statistics = Statistics()

        inline fun build(block: Builder.() -> Unit): Statistics = Builder().apply(block).build()
    }
}

fun Bundle.putStatistics(statistics: Statistics) {
    putLong(RX_BYTES_KEY, statistics.rxBytes)
    putLong(TX_BYTES_KEY, statistics.txBytes)
    putLong(LAST_HANDSHAKE_KEY, statistics.lastHandshake)
}

fun Bundle.getStatistics(): Statistics =
    Statistics.build {
        setRxBytes(getLong(RX_BYTES_KEY))
        setTxBytes(getLong(TX_BYTES_KEY))
        setLastHandshake(getLong(LAST_HANDSHAKE_KEY))
    }
