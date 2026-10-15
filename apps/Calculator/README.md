# Calculator3DS

A scientific calculator developed in C for AuroraOS on the Nintendo 3DS.

## 📖 About

Calculator3DS is a calculator application designed for AuroraOS, offering both basic arithmetic operations and advanced scientific functions.

The application features a touchscreen interface, physical button controls, calculation history, and memory functions.

## ✨ Features

### Basic Calculator
- Addition, subtraction, multiplication, and division.
- Decimal and negative numbers.
- Percentage calculations.
- Sign toggle.
- Delete and clear functions.
- Parentheses for complex expressions.
- Implicit multiplication (e.g. `2pi`, `2(3+4)`).

### Scientific Calculator
- Powers, squares, and cubes.
- Square roots and other root operations.
- Trigonometric functions: `sin`, `cos`, and `tan`.
- Inverse trigonometric functions: `asin`, `acos`, and `atan`.
- Hyperbolic functions: `sinh`, `cosh`, and `tanh`.
- Inverse hyperbolic functions.
- Natural logarithm (`ln`) and base-10 logarithm (`log10`).
- Exponential functions (`exp` and powers of 10).
- Factorials.
- Reciprocal calculations.
- Mathematical constants: π and e.
- Scientific notation using `*10^`.
- Pseudorandom number generation.

### Additional Features
- **DEG/RAD mode:** Switch between degrees and radians.
- **Memory functions:** M+, M−, MR, and MC.
- **Ans:** Reuse the result of the previous calculation.
- **Calculation history:** Stores the last 8 calculations.
- **Dual-screen interface:** Designed for the Nintendo 3DS.
- **Touchscreen support:** Interact directly with the interface.
- **Physical controls:** Navigate and operate the calculator using the console's buttons.

## 🎮 Controls

| Button | Function |
|---|---|
| Touchscreen | Interact with the interface |
| D-pad | Navigate the interface |
| A | Select |
| B | Delete |
| X | Open calculation history |
| Y | Switch between degrees and radians |
| START | Calculate the expression |
| L | Activate second-function mode on the scientific calculator |
| R | Switch between basic and scientific modes |
| SELECT | Clear calculator memory |

## 📥 Download

The `.bin` file can be found in the folder dedicated to this application within the repository.

## 🛠️ Development

Calculator3DS is written in **C** and developed for AuroraOS on the Nintendo 3DS.

The project uses:
- C SDK
- The AuroraOS application API (`aurora_app.h`)
- Standard mathematical functions from the C library
- Touchscreen input
- Physical button input

The source code is available in this repository.

## ⚠️ Limitations

- The calculation history stores up to 8 entries.
- Factorials are limited to integer values between 0 and 170.
- Some mathematical operations may produce domain errors for invalid inputs.
- Floating-point calculations are subject to the precision limits of the `double` data type.
- Compatibility may depend on the AuroraOS version.

## 🙏 Acknowledgements

Thanks to the **AuroraOS Team** for developing AuroraOS and providing the environment that makes applications like this possible.

- AuroraOS: https://github.com/TeamAuroraOS/AuroraOS

## 📄 License

Please refer to the license information provided in the repository. If no license is specified, the source code remains subject to the applicable copyright laws.

---

**Developed by [DanyCasy](https://github.com/DanyCasy)**

*Part of the Aurora_App project — applications developed for AuroraOS on the Nintendo 3DS.*
