# Configured ADC sampling

`USE_ADC` selects the configured input owner. `USE_ENERGY_ANALOG` selects it
implicitly; existing profiles do not enable either feature. The portable API is
`adc_service.h`, with native conversions behind `newmota/platform/adc.h`.

The owner reserves each physical pin with a generation-safe `adc` runtime token.
There are 16 configured input slots across all clients; the energy provider can use
at most eight. Configured GPIO/console/display/serial reservations remain exclusive.
Only configured inputs are sampled; energy drivers allocate no unconfigured
instances. Call `poll(now)` from the application task. One conversion is outstanding
at a time, with round-robin scheduling and at least 1 ms between conversion starts.
Native SDKs may synchronously perform a bounded single conversion (ESP-IDF) or
report busy until ready (the other families); no 32-sample wait runs in the loop.
Beken's SDK FIFO/open/close routines and Bouffalo's initial ADC trim retain their
native SDK timing behavior; the 10-ms application deadline cannot interrupt a
stuck SDK call.

A completed snapshot contains floor-mean millivolts and raw counts over 32 samples,
completion time, sequence, errors and reference provenance. Conversions exceeding
10 ms are cancelled; partial windows exceeding two seconds are discarded. Failed
conversion/cleanup invalidates the window. Unconfirmed cleanup quarantines the
slot and retains the pin, including across failed driver construction. Cancelling
a client never releases another client's pin or conversion. Snapshot validity
means a complete window; callers must check its timestamp for their freshness policy.
The current owner supports external single-ended inputs; differential/internal
battery/temperature inputs, DMA streaming and custom attenuation are not exposed.

| Family | Native route and reference |
| --- | --- |
| ESP-IDF | SDK `adc_oneshot_io_to_channel` resolves each chip's actual pads, one retained handle per ADC unit, default native bit width and 12-dB attenuation. SDK curve/line calibration is used when available; unsupported calibration falls back to nominal 3300-mV full scale and is labeled `nominal`. Other creation/conversion errors fail explicitly. SDK calibration may itself use a default reference when eFuse data is absent. ESP32 ADC2/Wi-Fi arbitration and C3 ADC2 errata remain native SDK restrictions. |
| Beken BK7231N | SDK channels 1..6 on GPIO26/24/23/28/22/21. The shared SARADC busy flag arbitrates with SDK consumers; IRQ-updated completion is read under the native interrupt lock, then the descriptor is closed before reuse. SDK `saradc_format_data` and `saradc_calculate` supply millivolts. |
| Beken BK7238 | SDK channels 1..6 on GPIO26/24/20/28/1/10, with native accuracy adjustment and `saradc_calculate`. Board routing/reservations can expose fewer pads. |
| Bouffalo BL602 | External CH0/1/2/3/4/5/7/9/10/11 on GPIO12/4/14/13/5/6/9/10/11/15 (CH6/8 reserved aliases), 16-bit conversions with 3.2-V reference, SDK trim/result conversion. FIFO readiness is polled without waiting. GPIO12's optional external-reference role is not enabled. |
| AmebaD | PB4..PB7 are channels 0..3, PB1..PB3 channels 4..6 (native pin numbers 36..39 and 33..35). SDK calibration converts FIFO results to millivolts. ADC trigger/FIFO state is shared by the one application owner. |
| Pico SDK | RP2040/RP2350A GPIO26..29; RP2350B GPIO40..47. Board topology can reserve more pins. Pico W/2 W GPIO29 is excluded because it is also the SDK Wi-Fi clock/VSYS pad. Reference is nominal 3300 mV, not factory-calibrated. |

`Reference=sdk` identifies SDK conversion/trim, not a measured accuracy guarantee.
Native input voltage ranges, reference rails, package bonds and external dividers
must be checked for the actual chip/board; the provider's 3300-mV normalization does
not make every physical ADC tolerate or measure 3.3 V. Controller handles are shared
within this owner, not with unrelated native code that reconfigures ADC hardware.
Build/selection evidence is not hardware qualification.

BL602 route mapping follows the module vendor’s [ADC channel table](https://docs.ai-thinker.com/en/ai-wb2_devel/basic/adc.html).
