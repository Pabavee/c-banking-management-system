# Banking Management System (C++17)

A menu-driven console banking application written in C++17 with no external
dependencies. It demonstrates encapsulation, inheritance, runtime
polymorphism, RAII/smart-pointer memory management, and durable file
persistence.

The project was originally delivered as a single `index.html` showcase page
with the C++ source embedded inside it. The source now lives in `main.cpp`,
and the showcase page is preserved at [`docs/index.html`](docs/index.html).

## Features

- Abstract `BankAccount` base class with `SavingsAccount` and
  `CheckingAccount` subclasses
- Encapsulated balance and transaction history behind validated
  `deposit()` / `withdraw()` operations
- Runtime polymorphism for per-account withdrawal rules, monthly interest,
  and overdraft fees
- RAII and `std::unique_ptr` ownership — no manual `new` / `delete`
- Automatic persistence to `bank_data.txt` after every successful
  transaction, using an atomic "write temp file, then rename" save
- Robust console input handling with `cin.clear()` / `cin.ignore()`

## Requirements

- A C++17 compiler: g++, clang++, or MSVC
- Optional: GNU Make or CMake

## Build and run

### g++ / clang++ (direct)

```sh
g++ -std=c++17 -Wall -Wextra -pedantic -O2 main.cpp -o bank
./bank
```

On Windows the command produces `bank.exe`; run it with `.\bank.exe`.
clang++ works the same way.

### Make

```sh
make
make run
```

Use `mingw32-make` instead of `make` if that is what your MinGW installation
provides. `make clean` removes the built executable.

### CMake

```sh
cmake -S . -B build
cmake --build build --config Release
```

The executable is written to `build/bank` for single-config generators
(Makefiles, Ninja) or `build/Release/bank.exe` for multi-config generators
(Visual Studio).

### MSVC

From an "x64 Native Tools Command Prompt for VS":

```bat
cl /std:c++17 /EHsc /W4 main.cpp /Fe:bank.exe
bank.exe
```

## Usage

The program presents a menu:

```
1. Create account
2. Deposit
3. Withdraw
4. Transfer funds
5. View account details & history
6. List all accounts
7. Run monthly update
0. Save and exit
```

Data is saved to `bank_data.txt` in the working directory after every
successful transaction and again on exit. The file is human-readable,
pipe-delimited text: `A|...` account records followed by `T|...` transaction
records. Delete the file to reset the application. It is created at runtime
and ignored by git.

## Project structure

```
.
├── main.cpp          # complete C++17 source
├── Makefile          # g++/clang++ build
├── CMakeLists.txt    # portable CMake build
├── README.md
├── .gitignore
└── docs/
    └── index.html    # original self-contained showcase page
```

The showcase page can be opened directly in a browser; its copy and download
buttons emit the same `main.cpp`.
