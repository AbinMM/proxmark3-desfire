//-----------------------------------------------------------------------------
// Routines to support ISO 14443. This includes both the reader software and
// the `fake tag' modes. At the moment only the Type B modulation is
// supported.
// Jonathan Westhues, split Nov 2006
//-----------------------------------------------------------------------------
#include <proxmark3.h>
#include "apps.h"
#include "../common/iso14443_crc.c"


//static void GetSamplesFor14443(BOOL weTx, int n);

#define DMA_BUFFER_SIZE 256

//=============================================================================
// An ISO 14443 Type B tag. We listen for commands from the reader, using
// a UART kind of thing that's implemented in software. When we get a
// frame (i.e., a group of bytes between SOF and EOF), we check the CRC.
// If it's good, then we can do something appropriate with it, and send
// a response.
//=============================================================================

//-----------------------------------------------------------------------------
// Code up a string of octets at layer 2 (including CRC, we don't generate
// that here) so that they can be transmitted to the reader. Doesn't transmit
// them yet, just leaves them ready to send in ToSend[].
//-----------------------------------------------------------------------------
static void CodeIso14443bAsTag(const BYTE *cmd, int len)
{
    int i;

    ToSendReset();

    // Transmit a burst of ones, as the initial thing that lets the
    // reader get phase sync. This (TR1) must be > 80/fs, per spec,
    // but tag that I've tried (a Paypass) exceeds that by a fair bit,
    // so I will too.
    for(i = 0; i < 20; i++) {
        ToSendStuffBit(1);
        ToSendStuffBit(1);
        ToSendStuffBit(1);
        ToSendStuffBit(1);
    }

    // Send SOF.
    for(i = 0; i < 10; i++) {
        ToSendStuffBit(0);
        ToSendStuffBit(0);
        ToSendStuffBit(0);
        ToSendStuffBit(0);
    }
    for(i = 0; i < 2; i++) {
        ToSendStuffBit(1);
        ToSendStuffBit(1);
        ToSendStuffBit(1);
        ToSendStuffBit(1);
    }

    for(i = 0; i < len; i++) {
        int j;
        BYTE b = cmd[i];

        // Start bit
        ToSendStuffBit(0);
        ToSendStuffBit(0);
        ToSendStuffBit(0);
        ToSendStuffBit(0);

        // Data bits
        for(j = 0; j < 8; j++) {
            if(b & 1) {
                ToSendStuffBit(1);
                ToSendStuffBit(1);
                ToSendStuffBit(1);
                ToSendStuffBit(1);
            } else {
                ToSendStuffBit(0);
                ToSendStuffBit(0);
                ToSendStuffBit(0);
                ToSendStuffBit(0);
            }
            b >>= 1;
        }

        // Stop bit
        ToSendStuffBit(1);
        ToSendStuffBit(1);
        ToSendStuffBit(1);
        ToSendStuffBit(1);
    }

    // Send SOF.
    for(i = 0; i < 10; i++) {
        ToSendStuffBit(0);
        ToSendStuffBit(0);
        ToSendStuffBit(0);
        ToSendStuffBit(0);
    }
    for(i = 0; i < 10; i++) {
        ToSendStuffBit(1);
        ToSendStuffBit(1);
        ToSendStuffBit(1);
        ToSendStuffBit(1);
    }

    // Convert from last byte pos to length
    ToSendMax++;

    // Add a few more for slop
    ToSendMax += 2;
}

//-----------------------------------------------------------------------------
// The software UART that receives commands from the reader, and its state
// variables.
//-----------------------------------------------------------------------------
static struct {
    enum {
        STATE_UNSYNCD,
        STATE_GOT_FALLING_EDGE_OF_SOF,
        STATE_AWAITING_START_BIT,
        STATE_RECEIVING_DATA,
        STATE_ERROR_WAIT
    }       state;
    WORD    shiftReg;
    int     bitCnt;
    int     byteCnt;
    int     byteCntMax;
    int     posCnt;
    BYTE   *output;
} Uart;

static BOOL Handle14443UartBit(int bit)
{
    switch(Uart.state) {
        case STATE_UNSYNCD:
            if(!bit) {
                // we went low, so this could be the beginning
                // of an SOF
                Uart.state = STATE_GOT_FALLING_EDGE_OF_SOF;
                Uart.posCnt = 0;
                Uart.bitCnt = 0;
            }
            break;

        case STATE_GOT_FALLING_EDGE_OF_SOF:
            Uart.posCnt++;
            if(Uart.posCnt == 2) {
                if(bit) {
                    if(Uart.bitCnt >= 10) {
                        // we've seen enough consecutive
                        // zeros that it's a valid SOF
                        Uart.posCnt = 0;
                        Uart.byteCnt = 0;
                        Uart.state = STATE_AWAITING_START_BIT;
                    } else {
                        // didn't stay down long enough
                        // before going high, error
                        Uart.state = STATE_ERROR_WAIT;
                    }
                } else {
                    // do nothing, keep waiting
                }
                Uart.bitCnt++;
            }
            if(Uart.posCnt >= 4) Uart.posCnt = 0;
            if(Uart.bitCnt > 14) {
                // Give up if we see too many zeros without
                // a one, too.
                Uart.state = STATE_ERROR_WAIT;
            }
            break;

        case STATE_AWAITING_START_BIT:
            Uart.posCnt++;
            if(bit) {
                if(Uart.posCnt > 25) {
                    // stayed high for too long between
                    // characters, error
                    Uart.state = STATE_ERROR_WAIT;
                }
            } else {
                // falling edge, this starts the data byte
                Uart.posCnt = 0;
                Uart.bitCnt = 0;
                Uart.shiftReg = 0;
                Uart.state = STATE_RECEIVING_DATA;
            }
            break;

        case STATE_RECEIVING_DATA:
            Uart.posCnt++;
            if(Uart.posCnt == 2) {
                // time to sample a bit
                Uart.shiftReg >>= 1;
                if(bit) {
                    Uart.shiftReg |= 0x200;
                }
                Uart.bitCnt++;
            }
            if(Uart.posCnt >= 4) {
                Uart.posCnt = 0;
            }
            if(Uart.bitCnt == 10) {
                if((Uart.shiftReg & 0x200) && !(Uart.shiftReg & 0x001))
                {
                    // this is a data byte, with correct
                    // start and stop bits
                    Uart.output[Uart.byteCnt] = (Uart.shiftReg >> 1) & 0xff;
                    Uart.byteCnt++;

                    if(Uart.byteCnt >= Uart.byteCntMax) {
                        // Buffer overflowed, give up
                        Uart.posCnt = 0;
                        Uart.state = STATE_ERROR_WAIT;
                    } else {
                        // so get the next byte now
                        Uart.posCnt = 0;
                        Uart.state = STATE_AWAITING_START_BIT;
                    }
                } else if(Uart.shiftReg == 0x000) {
                    // this is an EOF byte
                    return TRUE;
                } else {
                    // this is an error
                    Uart.posCnt = 0;
                    Uart.state = STATE_ERROR_WAIT;
                }
            }
            break;

        case STATE_ERROR_WAIT:
            // We're all screwed up, so wait a little while
            // for whatever went wrong to finish, and then
            // start over.
            Uart.posCnt++;
            if(Uart.posCnt > 10) {
                Uart.state = STATE_UNSYNCD;
            }
            break;

        default:
            Uart.state = STATE_UNSYNCD;
            break;
    }

    return FALSE;
}

//-----------------------------------------------------------------------------
// Receive a command (from the reader to us, where we are the simulated tag),
// and store it in the given buffer, up to the given maximum length. Keeps
// spinning, waiting for a well-framed command, until either we get one
// (returns TRUE) or someone presses the pushbutton on the board (FALSE).
//
// Assume that we're called with the SSC (to the FPGA) and ADC path set
// correctly.
//-----------------------------------------------------------------------------
static BOOL GetIso14443CommandFromReader(BYTE *received, int *len, int maxLen)
{
    BYTE mask;
    int i, bit;

    // Set FPGA mode to "simulated ISO 14443 tag", no modulation (listen
    // only, since we are receiving, not transmitting).
    FpgaWriteConfWord(
    	FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_NO_MODULATION);


    // Now run a `software UART' on the stream of incoming samples.
    Uart.output = received;
    Uart.byteCntMax = maxLen;
    Uart.state = STATE_UNSYNCD;

    for(;;) {
        WDT_HIT();

        if(BUTTON_PRESS()) return FALSE;

        if(SSC_STATUS & (SSC_STATUS_TX_READY)) {
            SSC_TRANSMIT_HOLDING = 0x00;
        }
        if(SSC_STATUS & (SSC_STATUS_RX_READY)) {
            BYTE b = (BYTE)SSC_RECEIVE_HOLDING;

            mask = 0x80;
            for(i = 0; i < 8; i++, mask >>= 1) {
                bit = (b & mask);
                if(Handle14443UartBit(bit)) {
                    *len = Uart.byteCnt;
                    return TRUE;
                }
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Main loop of simulated tag: receive commands from reader, decide what
// response to send, and send it.
//-----------------------------------------------------------------------------
void SimulateIso14443Tag(void)
{
    static const BYTE cmd1[] = { 0x05, 0x00, 0x08, 0x39, 0x73 };
    static const BYTE response1[] = {
        0x50, 0x82, 0x0d, 0xe1, 0x74, 0x20, 0x38, 0x19, 0x22,
        0x00, 0x21, 0x85, 0x5e, 0xd7
    };

    BYTE *resp;
    int respLen;

    BYTE *resp1 = (((BYTE *)BigBuf) + 800);
    int resp1Len;

    BYTE *receivedCmd = (BYTE *)BigBuf;
    int len;

    int i;

    int cmdsRecvd = 0;

    memset(receivedCmd, 0x44, 400);

    CodeIso14443bAsTag(response1, sizeof(response1));
    memcpy(resp1, ToSend, ToSendMax); resp1Len = ToSendMax;

    // We need to listen to the high-frequency, peak-detected path.
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);
    FpgaSetupSsc();

    cmdsRecvd = 0;

    for(;;) {
        BYTE b1, b2;

        if(!GetIso14443CommandFromReader(receivedCmd, &len, 100)) {
            DbpIntegers(cmdsRecvd, 0, 0);
            DbpString("button press");
            break;
        }

        // Good, look at the command now.

        if(len == sizeof(cmd1) && memcmp(receivedCmd, cmd1, len)==0) {
            resp = resp1; respLen = resp1Len;
        } else {
            DbpString("new cmd from reader:");
            DbpIntegers(len, 0x1234, cmdsRecvd);
            // And print whether the CRC fails, just for good measure
            ComputeCrc14443(CRC_14443_B, receivedCmd, len-2, &b1, &b2);
            if(b1 != receivedCmd[len-2] || b2 != receivedCmd[len-1]) {
                // Not so good, try again.
                DbpString("+++CRC fail");
            } else {
                DbpString("CRC passes");
            }
            break;
        }

        memset(receivedCmd, 0x44, 32);

        cmdsRecvd++;

        if(cmdsRecvd > 0x30) {
            DbpString("many commands later...");
            break;
        }

        if(respLen <= 0) continue;

        // Modulate BPSK
        FpgaWriteConfWord(
        	FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_MODULATE_BPSK);
        SSC_TRANSMIT_HOLDING = 0xff;
        FpgaSetupSsc();

        // Transmit the response.
        i = 0;
        for(;;) {
            if(SSC_STATUS & (SSC_STATUS_TX_READY)) {
                BYTE b = resp[i];

                SSC_TRANSMIT_HOLDING = b;

                i++;
                if(i > respLen) {
                    break;
                }
            }
            if(SSC_STATUS & (SSC_STATUS_RX_READY)) {
                volatile BYTE b = (BYTE)SSC_RECEIVE_HOLDING;
                (void)b;
            }
        }
    }
}

//=============================================================================
// An ISO 14443 Type B reader. We take layer two commands, code them
// appropriately, and then send them to the tag. We then listen for the
// tag's response, which we leave in the buffer to be demodulated on the
// PC side.
//=============================================================================

static struct {
    enum {
        DEMOD_UNSYNCD,
        DEMOD_PHASE_REF_TRAINING,
        DEMOD_AWAITING_FALLING_EDGE_OF_SOF,
        DEMOD_GOT_FALLING_EDGE_OF_SOF,
        DEMOD_AWAITING_START_BIT,
        DEMOD_RECEIVING_DATA,
        DEMOD_ERROR_WAIT
    }       state;
    int     bitCount;
    int     posCount;
    int     thisBit;
    int     metric;
    int     metricN;
    WORD    shiftReg;
    BYTE   *output;
    int     len;
    int     sumI;
    int     sumQ;
} Demod;

static BOOL Handle14443SamplesDemod(int ci, int cq)
{
    int v;

    // The soft decision on the bit uses an estimate of just the
    // quadrant of the reference angle, not the exact angle.
#define MAKE_SOFT_DECISION() { \
        if(Demod.sumI > 0) { \
            v = ci; \
        } else { \
            v = -ci; \
        } \
        if(Demod.sumQ > 0) { \
            v += cq; \
        } else { \
            v -= cq; \
        } \
    }

    switch(Demod.state) {
        case DEMOD_UNSYNCD:
            v = ci;
            if(v < 0) v = -v;
            if(cq > 0) {
                v += cq;
            } else {
                v -= cq;
            }
            if(v > 40) {
                Demod.posCount = 0;
                Demod.state = DEMOD_PHASE_REF_TRAINING;
                Demod.sumI = 0;
                Demod.sumQ = 0;
            }
            break;

        case DEMOD_PHASE_REF_TRAINING:
            if(Demod.posCount < 8) {
                Demod.sumI += ci;
                Demod.sumQ += cq;
            } else if(Demod.posCount > 100) {
                // error, waited too long
                Demod.state = DEMOD_UNSYNCD;
            } else {
                MAKE_SOFT_DECISION();
                if(v < 0) {
                    Demod.state = DEMOD_AWAITING_FALLING_EDGE_OF_SOF;
                    Demod.posCount = 0;
                }
            }
            Demod.posCount++;
            break;

        case DEMOD_AWAITING_FALLING_EDGE_OF_SOF:
            MAKE_SOFT_DECISION();
            if(v < 0) {
                Demod.state = DEMOD_GOT_FALLING_EDGE_OF_SOF;
                Demod.posCount = 0;
            } else {
                if(Demod.posCount > 100) {
                    Demod.state = DEMOD_UNSYNCD;
                }
            }
            Demod.posCount++;
            break;

        case DEMOD_GOT_FALLING_EDGE_OF_SOF:
            MAKE_SOFT_DECISION();
            if(v > 0) {
                if(Demod.posCount < 12) {
                    Demod.state = DEMOD_UNSYNCD;
                } else {
                    Demod.state = DEMOD_AWAITING_START_BIT;
                    Demod.posCount = 0;
                    Demod.len = 0;
                    Demod.metricN = 0;
                    Demod.metric = 0;
                }
            } else {
                if(Demod.posCount > 100) {
                    Demod.state = DEMOD_UNSYNCD;
                }
            }
            Demod.posCount++;
            break;

        case DEMOD_AWAITING_START_BIT:
            MAKE_SOFT_DECISION();
            if(v > 0) {
                if(Demod.posCount > 10) {
                    Demod.state = DEMOD_UNSYNCD;
                }
            } else {
                Demod.bitCount = 0;
                Demod.posCount = 1;
                Demod.thisBit = v;
                Demod.shiftReg = 0;
                Demod.state = DEMOD_RECEIVING_DATA;
            }
            break;

        case DEMOD_RECEIVING_DATA:
            MAKE_SOFT_DECISION();
            if(Demod.posCount == 0) {
                Demod.thisBit = v;
                Demod.posCount = 1;
            } else {
                Demod.thisBit += v;

                if(Demod.thisBit > 0) {
                    Demod.metric += Demod.thisBit;
                } else {
                    Demod.metric -= Demod.thisBit;
                }
                (Demod.metricN)++;

                Demod.shiftReg >>= 1;
                if(Demod.thisBit > 0) {
                    Demod.shiftReg |= 0x200;
                }

                Demod.bitCount++;
                if(Demod.bitCount == 10) {
                    WORD s = Demod.shiftReg;
                    if((s & 0x200) && !(s & 0x001)) {
                        BYTE b = (s >> 1);
                        Demod.output[Demod.len] = b;
                        Demod.len++;
                        Demod.state = DEMOD_AWAITING_START_BIT;
                    } else if(s == 0x000) {
                        // This is EOF
                        return TRUE;
                        Demod.state = DEMOD_UNSYNCD;
                    } else {
                        Demod.state = DEMOD_UNSYNCD;
                    }
                }
                Demod.posCount = 0;
            }
            break;

        default:
            Demod.state = DEMOD_UNSYNCD;
            break;
    }

    return FALSE;
}

static void GetSamplesFor14443Demod(BOOL weTx, int n, BOOL quiet)
{
    int max = 0;
    BOOL gotFrame = FALSE;

//#   define DMA_BUFFER_SIZE 8
    SBYTE *dmaBuf;

    int lastRxCounter;
    SBYTE *upTo;

    int ci, cq;

    int samples = 0;

    // Clear out the state of the "UART" that receives from the tag.
    memset(BigBuf, 0x44, 400);
    Demod.output = (BYTE *)BigBuf;
    Demod.len = 0;
    Demod.state = DEMOD_UNSYNCD;

    // And the UART that receives from the reader
    Uart.output = (((BYTE *)BigBuf) + 1024);
    Uart.byteCntMax = 100;
    Uart.state = STATE_UNSYNCD;

    // Setup for the DMA.
    dmaBuf = (SBYTE *)(BigBuf + 32);
    upTo = dmaBuf;
    lastRxCounter = DMA_BUFFER_SIZE;
    FpgaSetupSscDma((BYTE *)dmaBuf, DMA_BUFFER_SIZE);

    // And put the FPGA in the appropriate mode
    FpgaWriteConfWord(
    	FPGA_MAJOR_MODE_HF_READER_RX_XCORR | FPGA_HF_READER_RX_XCORR_848_KHZ |
    	(weTx ? 0 : FPGA_HF_READER_RX_XCORR_SNOOP));

    for(;;) {
        int behindBy = lastRxCounter - PDC_RX_COUNTER(SSC_BASE);
        if(behindBy > max) max = behindBy;

        LED_D_ON();
        while(((lastRxCounter-PDC_RX_COUNTER(SSC_BASE)) & (DMA_BUFFER_SIZE-1))
                    > 2)
        {
            ci = upTo[0];
            cq = upTo[1];
            upTo += 2;
            if(upTo - dmaBuf > DMA_BUFFER_SIZE) {
                upTo -= DMA_BUFFER_SIZE;
                PDC_RX_NEXT_POINTER(SSC_BASE) = (DWORD)upTo;
                PDC_RX_NEXT_COUNTER(SSC_BASE) = DMA_BUFFER_SIZE;
            }
            lastRxCounter -= 2;
            if(lastRxCounter <= 0) {
                lastRxCounter += DMA_BUFFER_SIZE;
            }

            samples += 2;

            Handle14443UartBit(1);
            Handle14443UartBit(1);

            if(Handle14443SamplesDemod(ci, cq)) {
                gotFrame = 1;
            }
        }
        LED_D_OFF();

        if(samples > 2000) {
            break;
        }
    }
    PDC_CONTROL(SSC_BASE) = PDC_RX_DISABLE;
    if (!quiet) DbpIntegers(max, gotFrame, Demod.len);
}

//-----------------------------------------------------------------------------
// Read the tag's response. We just receive a stream of slightly-processed
// samples from the FPGA, which we will later do some signal processing on,
// to get the bits.
//-----------------------------------------------------------------------------
/*static void GetSamplesFor14443(BOOL weTx, int n)
{
    BYTE *dest = (BYTE *)BigBuf;
    int c;

    FpgaWriteConfWord(
    	FPGA_MAJOR_MODE_HF_READER_RX_XCORR | FPGA_HF_READER_RX_XCORR_848_KHZ |
    	(weTx ? 0 : FPGA_HF_READER_RX_XCORR_SNOOP));

    c = 0;
    for(;;) {
        if(SSC_STATUS & (SSC_STATUS_TX_READY)) {
            SSC_TRANSMIT_HOLDING = 0x43;
        }
        if(SSC_STATUS & (SSC_STATUS_RX_READY)) {
            SBYTE b;
            b = (SBYTE)SSC_RECEIVE_HOLDING;

            dest[c++] = (BYTE)b;

            if(c >= n) {
                break;
            }
        }
    }
}*/

//-----------------------------------------------------------------------------
// Transmit the command (to the tag) that was placed in ToSend[].
//-----------------------------------------------------------------------------
static void TransmitFor14443(void)
{
    int c;

    FpgaSetupSsc();

    while(SSC_STATUS & (SSC_STATUS_TX_READY)) {
        SSC_TRANSMIT_HOLDING = 0xff;
    }

    FpgaWriteConfWord(
    	FPGA_MAJOR_MODE_HF_READER_TX | FPGA_HF_READER_TX_SHALLOW_MOD);

    for(c = 0; c < 10;) {
        if(SSC_STATUS & (SSC_STATUS_TX_READY)) {
            SSC_TRANSMIT_HOLDING = 0xff;
            c++;
        }
        if(SSC_STATUS & (SSC_STATUS_RX_READY)) {
            volatile DWORD r = SSC_RECEIVE_HOLDING;
            (void)r;
        }
        WDT_HIT();
    }

    c = 0;
    for(;;) {
        if(SSC_STATUS & (SSC_STATUS_TX_READY)) {
            SSC_TRANSMIT_HOLDING = ToSend[c];
            c++;
            if(c >= ToSendMax) {
                break;
            }
        }
        if(SSC_STATUS & (SSC_STATUS_RX_READY)) {
            volatile DWORD r = SSC_RECEIVE_HOLDING;
            (void)r;
        }
        WDT_HIT();
    }
}

//-----------------------------------------------------------------------------
// Code a layer 2 command (string of octets, including CRC) into ToSend[],
// so that it is ready to transmit to the tag using TransmitFor14443().
//-----------------------------------------------------------------------------
void CodeIso14443bAsReader(const BYTE *cmd, int len)
{
    int i, j;
    BYTE b;

    ToSendReset();

    // Establish initial reference level
    for(i = 0; i < 40; i++) {
        ToSendStuffBit(1);
    }
    // Send SOF
    for(i = 0; i < 10; i++) {
        ToSendStuffBit(0);
    }

    for(i = 0; i < len; i++) {
        // Stop bits/EGT
        ToSendStuffBit(1);
        ToSendStuffBit(1);
        // Start bit
        ToSendStuffBit(0);
        // Data bits
        b = cmd[i];
        for(j = 0; j < 8; j++) {
            if(b & 1) {
                ToSendStuffBit(1);
            } else {
                ToSendStuffBit(0);
            }
            b >>= 1;
        }
    }
    // Send EOF
    ToSendStuffBit(1);
    for(i = 0; i < 10; i++) {
        ToSendStuffBit(0);
    }
    for(i = 0; i < 8; i++) {
        ToSendStuffBit(1);
    }

    // And then a little more, to make sure that the last character makes
    // it out before we switch to rx mode.
    for(i = 0; i < 24; i++) {
        ToSendStuffBit(1);
    }

    // Convert from last character reference to length
    ToSendMax++;
}

//-----------------------------------------------------------------------------
// Read an ISO 14443 tag. We send it some set of commands, and record the
// responses.
// The command name is misleading, it actually decodes the reponse in HEX
// into the output buffer (read the result using hexsamples, not hisamples)
//-----------------------------------------------------------------------------
void AcquireRawAdcSamplesIso14443(DWORD parameter)
{
    BYTE cmd1[] = { 0x05, 0x00, 0x08, 0x39, 0x73 };

    // Make sure that we start from off, since the tags are stateful;
    // confusing things will happen if we don't reset them between reads.
    LED_D_OFF();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    SpinDelay(200);

    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);
    FpgaSetupSsc();

    // Now give it time to spin up.
    FpgaWriteConfWord(
    	FPGA_MAJOR_MODE_HF_READER_RX_XCORR | FPGA_HF_READER_RX_XCORR_848_KHZ);
    SpinDelay(200);

    CodeIso14443bAsReader(cmd1, sizeof(cmd1));
    TransmitFor14443();
    LED_A_ON();
    GetSamplesFor14443Demod(TRUE, 2000, FALSE);
    LED_A_OFF();
}

//-----------------------------------------------------------------------------
// Read a SRI512 ISO 14443 tag.
// 
// SRI512 tags are just simple memory tags, here we're looking at making a dump
// of the contents of the memory. No anticollision algorithm is done, we assume
// we have a single tag in the field.
//
// I tried to be systematic and check every answer of the tag, every CRC, etc...
//-----------------------------------------------------------------------------
void ReadSRI512Iso14443(DWORD parameter)
{
    BYTE i = 0x00;

    // Make sure that we start from off, since the tags are stateful;
    // confusing things will happen if we don't reset them between reads.
    LED_D_OFF();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    SpinDelay(200);

    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);
    FpgaSetupSsc();

    // Now give it time to spin up.
    FpgaWriteConfWord(
    	FPGA_MAJOR_MODE_HF_READER_RX_XCORR | FPGA_HF_READER_RX_XCORR_848_KHZ);
    SpinDelay(200);

    // First command: wake up the tag using the INITIATE command
    BYTE cmd1[] = { 0x06, 0x00, 0x97, 0x5b};
    CodeIso14443bAsReader(cmd1, sizeof(cmd1));
    TransmitFor14443();
    LED_A_ON();
    GetSamplesFor14443Demod(TRUE, 2000,TRUE);
    LED_A_OFF();

    if (Demod.len == 0) {
	DbpString("No response from tag");
	return;
    } else {
	DbpString("Randomly generated UID from tag (+ 2 byte CRC):");
	DbpIntegers(Demod.output[0], Demod.output[1],Demod.output[2]);
    }
    // There is a response, SELECT the uid
    DbpString("Now SELECT tag:");
    cmd1[0] = 0x0E; // 0x0E is SELECT
    cmd1[1] = Demod.output[0];
    ComputeCrc14443(CRC_14443_B, cmd1, 2, &cmd1[2], &cmd1[3]);
    CodeIso14443bAsReader(cmd1, sizeof(cmd1));
    TransmitFor14443();
    LED_A_ON();
    GetSamplesFor14443Demod(TRUE, 2000,TRUE);
    LED_A_OFF();
    if (Demod.len != 3) {
	DbpString("Expected 3 bytes from tag, got:");
	DbpIntegers(Demod.len,0x0,0x0);
	return;
    }
    // Check the CRC of the answer:
    ComputeCrc14443(CRC_14443_B, Demod.output, 1 , &cmd1[2], &cmd1[3]);
    if(cmd1[2] != Demod.output[1] || cmd1[3] != Demod.output[2]) {
	DbpString("CRC Error reading select response.");
	return;
    }
    // Check response from the tag: should be the same UID as the command we just sent:
    if (cmd1[1] != Demod.output[0]) {
	DbpString("Bad response to SELECT from Tag, aborting:");
	DbpIntegers(cmd1[1],Demod.output[0],0x0);
	return;
    }
    // Tag is now selected,
    // First get the tag's UID:
    cmd1[0] = 0x0B;
    ComputeCrc14443(CRC_14443_B, cmd1, 1 , &cmd1[1], &cmd1[2]);
    CodeIso14443bAsReader(cmd1, 3); // Only first three bytes for this one
    TransmitFor14443();
    LED_A_ON();
    GetSamplesFor14443Demod(TRUE, 2000,TRUE);
    LED_A_OFF();
    if (Demod.len != 10) {
	DbpString("Expected 10 bytes from tag, got:");
	DbpIntegers(Demod.len,0x0,0x0);
	return;
    }
    // The check the CRC of the answer (use cmd1 as temporary variable):
    ComputeCrc14443(CRC_14443_B, Demod.output, 8, &cmd1[2], &cmd1[3]);
           if(cmd1[2] != Demod.output[8] || cmd1[3] != Demod.output[9]) {
	DbpString("CRC Error reading block! - Below: expected, got");
	DbpIntegers( (cmd1[2]<<8)+cmd1[3], (Demod.output[8]<<8)+Demod.output[9],0);
	// Do not return;, let's go on... (we should retry, maybe ?)
    }
    DbpString("Tag UID (64 bits):");
    DbpIntegers((Demod.output[7]<<24) + (Demod.output[6]<<16) + (Demod.output[5]<<8) + Demod.output[4], (Demod.output[3]<<24) + (Demod.output[2]<<16) + (Demod.output[1]<<8) + Demod.output[0], 0);

    // Now loop to read all 16 blocks, address from 0 to 15
    DbpString("Tag memory dump, block 0 to 15");
    cmd1[0] = 0x08;
    i = 0x00;
    for (;;) {
	    if (i == 0x10) {
		    DbpString("System area block (0xff):");
		    i = 0xff;
	    }
	    cmd1[1] = i;
	    ComputeCrc14443(CRC_14443_B, cmd1, 2, &cmd1[2], &cmd1[3]);
	    CodeIso14443bAsReader(cmd1, sizeof(cmd1));
	    TransmitFor14443();
	    LED_A_ON();
	    GetSamplesFor14443Demod(TRUE, 2000,TRUE);
	    LED_A_OFF();
	    if (Demod.len != 6) { // Check if we got an answer from the tag
		DbpString("Expected 6 bytes from tag, got less...");
		return;
	    }
	    // The check the CRC of the answer (use cmd1 as temporary variable):
	    ComputeCrc14443(CRC_14443_B, Demod.output, 4, &cmd1[2], &cmd1[3]);
            if(cmd1[2] != Demod.output[4] || cmd1[3] != Demod.output[5]) {
		DbpString("CRC Error reading block! - Below: expected, got");
		DbpIntegers( (cmd1[2]<<8)+cmd1[3], (Demod.output[4]<<8)+Demod.output[5],0);
		// Do not return;, let's go on... (we should retry, maybe ?)
	    }
	    // Now print out the memory location:
	    DbpString("Address , Contents, CRC");
	    DbpIntegers(i, (Demod.output[3]<<24) + (Demod.output[2]<<16) + (Demod.output[1]<<8) + Demod.output[0], (Demod.output[4]<<8)+Demod.output[5]);
	    if (i == 0xff) {
		break;
	    }
	    i++;
    }
}


//=============================================================================
// Finally, the `sniffer' combines elements from both the reader and
// simulated tag, to show both sides of the conversation.
//=============================================================================

//-----------------------------------------------------------------------------
// Record the sequence of commands sent by the reader to the tag, with
// triggering so that we start recording at the point that the tag is moved
// near the reader.
//-----------------------------------------------------------------------------
void SnoopIso14443(void)
{
    // We won't start recording the frames that we acquire until we trigger;
    // a good trigger condition to get started is probably when we see a
    // response from the tag.
    BOOL triggered = FALSE;

    // The command (reader -> tag) that we're working on receiving.
    BYTE *receivedCmd = (((BYTE *)BigBuf) + 1024);
    // The response (tag -> reader) that we're working on receiving.
    BYTE *receivedResponse = (((BYTE *)BigBuf) + 1536);

    // As we receive stuff, we copy it from receivedCmd or receivedResponse
    // into trace, along with its length and other annotations.
    BYTE *trace = (BYTE *)BigBuf;
    int traceLen = 0;

    // The DMA buffer, used to stream samples from the FPGA.
//#   define DMA_BUFFER_SIZE 256
    SBYTE *dmaBuf = ((SBYTE *)BigBuf) + 2048;
    int lastRxCounter;
    SBYTE *upTo;
    int ci, cq;
    int maxBehindBy = 0;

    // Count of samples received so far, so that we can include timing
    // information in the trace buffer.
    int samples = 0;

    memset(trace, 0x44, 1000);

    // Set up the demodulator for tag -> reader responses.
    Demod.output = receivedResponse;
    Demod.len = 0;
    Demod.state = DEMOD_UNSYNCD;

    // And the reader -> tag commands
    memset(&Uart, 0, sizeof(Uart));
    Uart.output = receivedCmd;
    Uart.byteCntMax = 100;
    Uart.state = STATE_UNSYNCD;

    // And put the FPGA in the appropriate mode
    FpgaWriteConfWord(
    	FPGA_MAJOR_MODE_HF_READER_RX_XCORR | FPGA_HF_READER_RX_XCORR_848_KHZ |
    	FPGA_HF_READER_RX_XCORR_SNOOP);
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

    // Setup for the DMA.
    FpgaSetupSsc();
    upTo = dmaBuf;
    lastRxCounter = DMA_BUFFER_SIZE;
    FpgaSetupSscDma((BYTE *)dmaBuf, DMA_BUFFER_SIZE);

    LED_A_ON();

    // And now we loop, receiving samples.
    for(;;) {
        int behindBy = (lastRxCounter - PDC_RX_COUNTER(SSC_BASE)) &
                                (DMA_BUFFER_SIZE-1);
        if(behindBy > maxBehindBy) {
            maxBehindBy = behindBy;
            if(behindBy > 100) {
                DbpString("blew circular buffer!");
                goto done;
            }
        }
        if(behindBy < 2) continue;

        ci = upTo[0];
        cq = upTo[1];
        upTo += 2;
        lastRxCounter -= 2;
        if(upTo - dmaBuf > DMA_BUFFER_SIZE) {
            upTo -= DMA_BUFFER_SIZE;
            lastRxCounter += DMA_BUFFER_SIZE;
            PDC_RX_NEXT_POINTER(SSC_BASE) = (DWORD)upTo;
            PDC_RX_NEXT_COUNTER(SSC_BASE) = DMA_BUFFER_SIZE;
        }

        samples += 2;

#define HANDLE_BIT_IF_BODY \
            if(triggered) { \
                trace[traceLen++] = ((samples >>  0) & 0xff); \
                trace[traceLen++] = ((samples >>  8) & 0xff); \
                trace[traceLen++] = ((samples >> 16) & 0xff); \
                trace[traceLen++] = ((samples >> 24) & 0xff); \
                trace[traceLen++] = 0; \
                trace[traceLen++] = 0; \
                trace[traceLen++] = 0; \
                trace[traceLen++] = 0; \
                trace[traceLen++] = Uart.byteCnt; \
                memcpy(trace+traceLen, receivedCmd, Uart.byteCnt); \
                traceLen += Uart.byteCnt; \
                if(traceLen > 1000) break; \
            } \
            /* And ready to receive another command. */ \
            memset(&Uart, 0, sizeof(Uart)); \
            Uart.output = receivedCmd; \
            Uart.byteCntMax = 100; \
            Uart.state = STATE_UNSYNCD; \
            /* And also reset the demod code, which might have been */ \
            /* false-triggered by the commands from the reader. */ \
            memset(&Demod, 0, sizeof(Demod)); \
            Demod.output = receivedResponse; \
            Demod.state = DEMOD_UNSYNCD; \

        if(Handle14443UartBit(ci & 1)) {
            HANDLE_BIT_IF_BODY
        }
        if(Handle14443UartBit(cq & 1)) {
            HANDLE_BIT_IF_BODY
        }

        if(Handle14443SamplesDemod(ci, cq)) {
            // timestamp, as a count of samples
            trace[traceLen++] = ((samples >>  0) & 0xff);
            trace[traceLen++] = ((samples >>  8) & 0xff);
            trace[traceLen++] = ((samples >> 16) & 0xff);
            trace[traceLen++] = 0x80 | ((samples >> 24) & 0xff);
            // correlation metric (~signal strength estimate)
            if(Demod.metricN != 0) {
                Demod.metric /= Demod.metricN;
            }
            trace[traceLen++] = ((Demod.metric >>  0) & 0xff);
            trace[traceLen++] = ((Demod.metric >>  8) & 0xff);
            trace[traceLen++] = ((Demod.metric >> 16) & 0xff);
            trace[traceLen++] = ((Demod.metric >> 24) & 0xff);
            // length
            trace[traceLen++] = Demod.len;
            memcpy(trace+traceLen, receivedResponse, Demod.len);
            traceLen += Demod.len;
            if(traceLen > 1000) break;

            triggered = TRUE;
            LED_A_OFF();
            LED_B_ON();

            // And ready to receive another response.
            memset(&Demod, 0, sizeof(Demod));
            Demod.output = receivedResponse;
            Demod.state = DEMOD_UNSYNCD;
        }

        if(BUTTON_PRESS()) {
            DbpString("cancelled");
            goto done;
        }
    }

    DbpString("in done pt");

    DbpIntegers(maxBehindBy, Uart.state, Uart.byteCnt);
    DbpIntegers(Uart.byteCntMax, traceLen, 0x23);

done:
    PDC_CONTROL(SSC_BASE) = PDC_RX_DISABLE;
    LED_A_OFF();
    LED_B_OFF();
}
