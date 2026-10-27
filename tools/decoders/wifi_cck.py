"""Noncoherent CCK codeword detection at 44 MSps (four samples per chip).

Original implementation from the modulation equations, not a vendor decoder.
Reference: IEEE P802.11b/D3.1, 18.4.6.5, Tables 12–14:
https://www.ieee802.org/11/Documents/DocumentArchives/1999_docs/90845b_p80211b-draft3.1.pdf
No expected MAC bytes or CRC-guided bit repair are used.
"""
import numpy as np


def codebook(rate):
    if rate == 5.5:
        labels = np.array([[0, 0], [0, 1], [1, 0], [1, 1]], dtype=np.int8)
        words = np.array([[1j, 1, 1j, -1, 1j, 1, -1j, 1],
                          [-1j, -1, -1j, 1, 1j, 1, -1j, 1],
                          [-1j, 1, -1j, -1, -1j, 1, 1j, 1],
                          [1j, -1, 1j, 1, -1j, 1, 1j, 1]], dtype=np.complex64)
    elif rate == 11:
        labels = ((np.arange(64)[:, None] >> np.arange(6)) & 1).astype(np.int8)
        phases = (2 * labels[:, ::2] + labels[:, 1::2]) * (np.pi / 2)
        p2, p3, p4 = phases.T
        words = np.exp(1j * np.array([p2 + p3 + p4, p3 + p4, p2 + p4,
            p4, p2 + p3, p3, p2, np.zeros(64)]).T).astype(np.complex64)
        words[:, [3, 6]] *= -1
    else:
        raise ValueError('CCK supports 5.5 and 11 Mbps')
    return words, labels


BOOKS = {rate: codebook(rate) for rate in (5.5, 11)}
DQPSK_BITS = np.array([[0, 0], [0, 1], [1, 1], [1, 0]], dtype=np.int8)


def decode(y, start, rate, octets, carrier_phase, carrier_origin, last_header):
    """Return scrambled payload bits; prior Barker symbol supplies phase reference.

    Length is determined by the CRC-valid PLCP header. Missing samples are not
    reconstructed. The best codeword magnitude sets each symbol's six/two code
    bits, while its projection phase supplies the differential first dibit.
    """
    words, labels = BOOKS[rate]
    count = octets * (2 if rate == 5.5 else 1)
    end = start + count * 32
    if octets < 1 or end > len(y):
        return np.empty(0, dtype=np.int8)
    samples = y[start:end] * np.exp(-1j * carrier_phase *
                                   (np.arange(start, end) - carrier_origin) / 44)
    chips = samples.reshape(count, 8, 4).mean(axis=2)
    correlations = chips @ words.conj().T
    choices = np.argmax(np.abs(correlations), axis=1)
    phases = correlations[np.arange(count), choices]
    # Barker's correlation contains the carrier phase at its 44-sample midpoint.
    last_header *= np.exp(-1j * carrier_phase * 21.5 / 44)
    previous = np.r_[last_header, phases[:-1]]
    changes = np.rint(np.angle(phases * previous.conj()) / (np.pi / 2)).astype(int)
    changes = (changes - 2 * (np.arange(count) % 2)) % 4
    return np.column_stack([DQPSK_BITS[changes], labels[choices]]).ravel()
