# Roller ECU firmware foundation

This STM32CubeMX 6.18 / STM32CubeH5 1.7 project targets the custom
STM32H563ZIT6 ECU PCB. TrustZone separates the safety-related ADC, valve PWM,
E-stop, I2C and watchdog service from the NonSecure communication application.

Important project documentation:

- [KiCad/CubeMX audit and manual actions](docs/ECU_CubeMX_KiCad_Audit.md)
- [CubeMX configuration source](ECU.ioc)
- [Secure safety API](Secure_nsclib/safety_api.h)

Common checks:

```sh
./tools/audit_config.sh
./tools/build.sh Debug
./tools/build.sh Release
./tools/cubemx_cli.sh validate
./tools/cubemx_cli.sh generate
```

`cubemx_cli.sh` defaults to the locally installed STM32CubeMX path. Override it
with `CUBEMX_BIN` when the installation is elsewhere.
