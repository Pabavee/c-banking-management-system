// ============================================================================
//  main.cpp  -  Console Banking Management System
// ----------------------------------------------------------------------------
//  Demonstrates:
//    * Encapsulation  - account state is private/protected, reachable only
//                       through a validated public interface.
//    * Inheritance    - SavingsAccount and CheckingAccount extend BankAccount.
//    * Polymorphism   - the Bank works with BankAccount* / unique_ptr and the
//                       correct rules run via virtual dispatch.
//    * Memory mgmt    - RAII + std::unique_ptr, so there is no manual new/delete.
//    * Persistence    - std::ifstream / std::ofstream, atomic "write temp then
//                       rename" saving, tolerant parsing on load.
//    * Robust input   - cin.clear() + cin.ignore() to recover from bad input.
//
//  Build:  g++ -std=c++17 -Wall -Wextra -pedantic -O2 main.cpp -o bank
//  Run:    ./bank
// ============================================================================

#include <algorithm>   // std::transform
#include <cctype>      // std::toupper
#include <cmath>       // std::round, std::fabs, std::isfinite
#include <cstdio>      // std::remove, std::rename (file replacement on save)
#include <ctime>       // std::time, std::strftime, std::localtime
#include <fstream>     // std::ifstream, std::ofstream  (data persistence)
#include <iomanip>     // std::setw, std::setprecision, std::left
#include <iostream>    // std::cin, std::cout
#include <limits>      // std::numeric_limits (used with cin.ignore)
#include <memory>      // std::unique_ptr, std::make_unique
#include <sstream>     // std::ostringstream, std::istringstream
#include <stdexcept>   // std::runtime_error
#include <string>
#include <vector>

// ============================================================================
//  SECTION 1 - Small utilities
//  Grouped in a namespace so they do not pollute the global scope, and so a
//  reader immediately sees "these are free helper functions, not part of any
//  class".
// ============================================================================
namespace util {

// Money is stored as double here for readability. Production banking software
// would use integer cents (int64_t) or a decimal type, because binary
// floating point cannot represent 0.10 exactly. To limit drift we round to
// 2 decimal places after every arithmetic operation.
inline double roundCents(double v) { return std::round(v * 100.0) / 100.0; }

// Formats a number as "$1234.50" or "-$20.00".
std::string money(double v) {
    v = roundCents(v);
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << std::fabs(v);
    return (v < 0 ? "-$" : "$") + os.str();
}

// Returns the current local time as "YYYY-MM-DD HH:MM:SS".
// std::localtime returns a pointer to a static buffer (not thread-safe), which
// is fine for a single-threaded console app. Use localtime_r / localtime_s in
// multi-threaded code.
std::string timestamp() {
    std::time_t now = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return buf;
}

// Splits "a|b|c" into {"a","b","c"}. Used when parsing the data file.
// The input is taken by const reference: no copy of a potentially long line.
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::string token;
    std::istringstream stream(s);
    while (std::getline(stream, token, delim)) parts.push_back(token);
    return parts;
}

// Strips leading/trailing whitespace.
std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Upper-cases a string. The lambda takes unsigned char because passing a
// negative char to std::toupper is undefined behaviour.
std::string toUpper(std::string s) {  // by value: we modify our own copy
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// Fixed-point formatting helper for serialising rates / balances.
std::string fixed(double v, int precision) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(precision) << v;
    return os.str();
}

}  // namespace util

// ============================================================================
//  SECTION 2 - Validated console input
//  Every read from std::cin goes through these functions, so the rest of the
//  program never has to worry about malformed input.
// ============================================================================
namespace input {

// Thrown when stdin is closed (Ctrl+D / Ctrl+Z or piped input ended).
// Without this, a "retry until valid" loop would spin forever on EOF.
struct InputClosed : std::runtime_error {
    InputClosed() : std::runtime_error("input stream closed") {}
};

// Reads a number of type T (int or double) within [minV, maxV].
//
// Why a template? One implementation serves both int menu choices and double
// money amounts, with full type safety and no code duplication.
//
// Typical failure: the user types "abc" when a number is expected.
//   1. operator>> puts cin into a fail state and sets failbit.
//   2. While failbit is set, every further read silently does nothing.
//   3. So we must cin.clear() (reset the flags) AND cin.ignore() (throw away the
//      bad characters still sitting in the buffer). Skipping either one causes
//      an infinite loop or a skipped prompt.
template <typename T>
T readNumber(const std::string& prompt, T minV, T maxV, const std::string& rangeMsg) {
    while (true) {
        std::cout << prompt;
        T value{};
        if (std::cin >> value) {
            // Swallow trailing spaces, then require end-of-line. This rejects
            // inputs like "12abc" or "5.5" for an int, which operator>> alone
            // would partially accept.
            while (std::cin.peek() == ' ' || std::cin.peek() == '\t') std::cin.get();
            if (std::cin.peek() == '\n') {
                // Remove the leftover newline so the next getline() starts clean.
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                // isfinite blocks "nan" and "inf", which operator>> accepts for doubles.
                if (std::isfinite(static_cast<double>(value)) && value >= minV && value <= maxV)
                    return value;
                std::cout << "  [!] " << rangeMsg << "\n";
                continue;
            }
        }
        if (std::cin.eof()) throw InputClosed();  // nothing more to read, so stop retrying
        std::cin.clear();                                                    // reset the error flags
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');  // discard the bad line
        std::cout << "  [!] Invalid input. Please enter a valid number.\n";
    }
}

// Reads a whole line (spaces allowed). Used for names and account numbers.
std::string readLine(const std::string& prompt) {
    std::cout << prompt;
    std::string line;
    if (!std::getline(std::cin, line)) throw InputClosed();
    return util::trim(line);
}

// Reads a non-empty owner name. The '|' character is the file delimiter, so
// allowing it would corrupt the data file. Rejecting it up front is simpler
// and safer than escaping.
std::string readName(const std::string& prompt) {
    while (true) {
        std::string name = readLine(prompt);
        if (name.empty())                          { std::cout << "  [!] Name cannot be empty.\n"; continue; }
        if (name.find('|') != std::string::npos)   { std::cout << "  [!] The '|' character is not allowed.\n"; continue; }
        if (name.size() > 40)                      { std::cout << "  [!] Name is too long (max 40 characters).\n"; continue; }
        return name;
    }
}

// Reads a money amount >= minAmount (and <= 1 billion as a sanity cap).
double readAmount(const std::string& prompt, double minAmount) {
    return util::roundCents(readNumber<double>(
        prompt, minAmount, 1e9,
        "Value out of range. Enter an amount of at least " + util::money(minAmount) + " (max $1,000,000,000)."));
}

void pause() { readLine("\nPress Enter to continue..."); }

}  // namespace input

// ============================================================================
//  SECTION 3 - Domain model
// ============================================================================

// A plain data record (a "struct", since it has no invariants to protect).
// The amount is SIGNED: deposits are positive, withdrawals are negative.
struct Transaction {
    std::string timestamp;
    std::string type;      // DEPOSIT, WITHDRAW, INTEREST, FEE, ...
    double amount;
    double balanceAfter;
    std::string note;
};

// ----------------------------------------------------------------------------
//  BankAccount - ABSTRACT base class.
//
//  It is abstract because it declares pure virtual functions (= 0). That makes
//  "a generic account" impossible to instantiate, which is correct: in the
//  real world every account is some specific kind.
// ----------------------------------------------------------------------------
class BankAccount {
public:
    BankAccount(std::string number, std::string owner, double openingBalance)
        : number_(std::move(number)),    // std::move: steal the string's buffer
          owner_(std::move(owner)),      // instead of copying it
          balance_(openingBalance) {}

    // VIRTUAL DESTRUCTOR - the single most important line for memory safety in
    // a polymorphic hierarchy. The Bank deletes accounts through a
    // BankAccount pointer. Without "virtual", only ~BankAccount() would run
    // and the derived part of the object would be destroyed incorrectly
    // (undefined behaviour).
    virtual ~BankAccount() = default;

    // Accounts represent unique real-world entities, so copying one makes no
    // sense (two objects with the same account number!). Deleting the copy
    // operations makes the compiler enforce that. Accounts are only ever
    // handled through unique_ptr.
    BankAccount(const BankAccount&) = delete;
    BankAccount& operator=(const BankAccount&) = delete;

    // ---- Accessors ---------------------------------------------------------
    // Returning const references avoids copying strings, and "const" on the
    // method promises the call will not modify the object (usable on const
    // objects too).
    const std::string& number() const { return number_; }
    const std::string& owner() const { return owner_; }
    double balance() const { return balance_; }
    const std::vector<Transaction>& history() const { return history_; }

    // ---- Public operations (TEMPLATE METHOD pattern) -----------------------
    // deposit() and withdraw() are NOT virtual. The common workflow
    // (validate -> check rules -> change balance -> record) is written once
    // here, and only the variable "rule" step is delegated to virtual
    // functions. That gives us reuse without duplicated code in subclasses.
    //
    // The error text comes back through a non-const reference parameter
    // ("out-parameter"), so the function can return a simple bool
    // success/failure and still tell the caller why it failed.
    bool deposit(double amount, const std::string& note, std::string& error) {
        if (!std::isfinite(amount) || amount <= 0.0) {
            error = "Deposit amount must be greater than zero.";
            return false;
        }
        applyAdjustment(+amount, "DEPOSIT", note);
        return true;
    }

    bool withdraw(double amount, const std::string& note, std::string& error) {
        if (!std::isfinite(amount) || amount <= 0.0) {
            error = "Withdrawal amount must be greater than zero.";
            return false;
        }
        // POLYMORPHISM: at runtime this calls Savings::canWithdraw or
        // Checking::canWithdraw depending on the real type of *this.
        if (!canWithdraw(amount, error)) return false;
        applyAdjustment(-amount, "WITHDRAW", note);
        return true;
    }

    // ---- Pure virtual interface: each account type MUST supply these -------
    virtual bool canWithdraw(double amount, std::string& error) const = 0;
    virtual void applyMonthlyUpdate() = 0;           // interest or fees
    virtual std::string typeName() const = 0;        // "SAVINGS" / "CHECKING"
    virtual double availableFunds() const = 0;       // spendable amount

    // Virtual with a default implementation. Derived classes override it,
    // call BankAccount::printDetails() first, then add their own lines.
    virtual void printDetails() const {
        std::cout << "  Account Number : " << number_ << "\n"
                  << "  Account Type   : " << typeName() << "\n"
                  << "  Owner          : " << owner_ << "\n"
                  << "  Balance        : " << util::money(balance_) << "\n"
                  << "  Available      : " << util::money(availableFunds()) << "\n";
    }

    void printHistory() const {
        std::cout << "\n  Transaction history (" << history_.size() << ")\n";
        if (history_.empty()) { std::cout << "  (no transactions yet)\n"; return; }
        std::cout << "  " << std::left << std::setw(20) << "Date/Time"
                  << std::setw(10) << "Type" << std::right
                  << std::setw(14) << "Amount" << std::setw(14) << "Balance" << "  Note\n";
        std::cout << "  " << std::string(78, '-') << "\n";
        for (const Transaction& t : history_) {  // const& - iterate without copying
            std::cout << "  " << std::left << std::setw(20) << t.timestamp
                      << std::setw(10) << t.type << std::right
                      << std::setw(14) << util::money(t.amount)
                      << std::setw(14) << util::money(t.balanceAfter)
                      << "  " << t.note << "\n";
        }
        std::cout << std::left;  // restore default alignment for later output
    }

    // ---- Persistence -------------------------------------------------------
    // Writes this account and all of its transactions. Taking std::ostream&
    // (the base class of ofstream) means the same code can write to a file,
    // a stringstream (for unit tests) or std::cout.
    void serialize(std::ostream& out) const {
        out << "A|" << typeName() << '|' << number_ << '|' << owner_ << '|'
            << util::fixed(balance_, 2) << '|' << extraField() << '\n';
        for (const Transaction& t : history_) {
            out << "T|" << number_ << '|' << t.timestamp << '|' << t.type << '|'
                << util::fixed(t.amount, 2) << '|' << util::fixed(t.balanceAfter, 2) << '|'
                << t.note << '\n';
        }
    }

    // Used while loading: re-attach a stored transaction WITHOUT changing the
    // balance (the balance was already restored from the account record).
    void restoreTransaction(Transaction t) { history_.push_back(std::move(t)); }

protected:
    // "protected": invisible to outside code, but derived classes may use it.
    // The balance is still never modified directly from outside; every change
    // is funnelled through this one function, so a history entry is ALWAYS
    // written together with a balance change (an invariant of the class).
    void applyAdjustment(double signedAmount, const std::string& type, const std::string& note) {
        balance_ = util::roundCents(balance_ + signedAmount);
        history_.push_back({util::timestamp(), type, signedAmount, balance_, note});
    }

    // Subclass-specific value stored in the last column of the data file.
    virtual std::string extraField() const = 0;

    double balance_;  // protected so derived classes can read it in their rule checks

private:
    // private: identity data can never change after creation.
    std::string number_;
    std::string owner_;
    std::vector<Transaction> history_;
};

// ----------------------------------------------------------------------------
//  SavingsAccount - earns interest, but must keep a minimum balance.
// ----------------------------------------------------------------------------
class SavingsAccount : public BankAccount {
public:
    static constexpr double kMinBalance = 100.0;     // compile-time constants shared by all instances
    static constexpr double kDefaultRate = 0.025;    // 2.5% APR

    SavingsAccount(std::string number, std::string owner, double balance, double annualRate = kDefaultRate)
        : BankAccount(std::move(number), std::move(owner), balance), annualRate_(annualRate) {}

    // "override" asks the compiler to verify we really override a base
    // virtual. If we mistype the signature, compilation fails instead of
    // silently creating an unrelated function.
    bool canWithdraw(double amount, std::string& error) const override {
        // 1e-9 tolerance guards against floating-point noise in the comparison.
        if (balance_ - amount < kMinBalance - 1e-9) {
            const double maxOut = std::max(0.0, balance_ - kMinBalance);
            error = "Savings accounts must keep a minimum balance of " + util::money(kMinBalance) +
                    ". Maximum you can withdraw now: " + util::money(maxOut) + ".";
            return false;
        }
        return true;
    }

    void applyMonthlyUpdate() override {
        const double interest = util::roundCents(balance_ * annualRate_ / 12.0);
        if (interest > 0.0)
            applyAdjustment(+interest, "INTEREST",
                            "Monthly interest @ " + util::fixed(annualRate_ * 100.0, 2) + "% APR");
    }

    std::string typeName() const override { return "SAVINGS"; }
    double availableFunds() const override { return std::max(0.0, balance_ - kMinBalance); }

    void printDetails() const override {
        BankAccount::printDetails();  // reuse the base implementation, then extend it
        std::cout << "  Interest Rate  : " << util::fixed(annualRate_ * 100.0, 2) << "% APR\n"
                  << "  Minimum Balance: " << util::money(kMinBalance) << "\n";
    }

protected:
    std::string extraField() const override { return util::fixed(annualRate_, 4); }

private:
    double annualRate_;
};

// ----------------------------------------------------------------------------
//  CheckingAccount - allows overdraft up to a limit, charges a fee on debt.
// ----------------------------------------------------------------------------
class CheckingAccount : public BankAccount {
public:
    static constexpr double kDefaultOverdraft = 500.0;
    static constexpr double kOverdraftFeeRate = 0.015;  // 1.5% of the negative balance per month

    CheckingAccount(std::string number, std::string owner, double balance, double overdraftLimit = kDefaultOverdraft)
        : BankAccount(std::move(number), std::move(owner), balance), overdraftLimit_(overdraftLimit) {}

    bool canWithdraw(double amount, std::string& error) const override {
        if (balance_ - amount < -overdraftLimit_ - 1e-9) {
            error = "Exceeds overdraft limit of " + util::money(overdraftLimit_) +
                    ". Maximum you can withdraw now: " + util::money(availableFunds()) + ".";
            return false;
        }
        return true;
    }

    void applyMonthlyUpdate() override {
        if (balance_ < 0.0) {
            const double fee = util::roundCents(-balance_ * kOverdraftFeeRate);
            if (fee > 0.0)
                applyAdjustment(-fee, "FEE",
                                "Overdraft fee @ " + util::fixed(kOverdraftFeeRate * 100.0, 1) + "%");
        }
    }

    std::string typeName() const override { return "CHECKING"; }
    double availableFunds() const override { return balance_ + overdraftLimit_; }

    void printDetails() const override {
        BankAccount::printDetails();
        std::cout << "  Overdraft Limit: " << util::money(overdraftLimit_) << "\n";
    }

protected:
    std::string extraField() const override { return util::fixed(overdraftLimit_, 2); }

private:
    double overdraftLimit_;
};

// ============================================================================
//  SECTION 4 - Bank: owns all accounts, implements the menu and persistence
// ============================================================================
class Bank {
public:
    explicit Bank(std::string dataFile) : filename_(std::move(dataFile)) {}
    // No destructor needed (the "Rule of Zero"): the vector of unique_ptr
    // destroys every account automatically, in the right order, even if an
    // exception is thrown. That is RAII at work.

    // ------------------------------------------------------------------ load
    void load() {
        std::ifstream in(filename_);
        if (!in) {  // first run: nothing to load yet
            std::cout << "No data file found - starting with an empty bank.\n";
            return;
        }
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            line = util::trim(line);
            if (line.empty() || line[0] == '#') continue;  // skip blanks and comments

            // A corrupted line must not crash the program or make us lose the
            // rest of the data, so each record is parsed inside its own
            // try/catch and bad ones are skipped with a warning.
            try {
                const auto f = util::split(line, '|');
                if (f[0] == "A") {
                    if (f.size() < 6) throw std::runtime_error("account record has too few fields");
                    const double balance = std::stod(f[4]);   // throws std::invalid_argument on garbage
                    const double extra = std::stod(f[5]);
                    // FACTORY: build the right concrete type, store it as the
                    // abstract base type. The unique_ptr<BankAccount> is
                    // where polymorphism begins.
                    std::unique_ptr<BankAccount> account;
                    if (f[1] == "SAVINGS")       account = std::make_unique<SavingsAccount>(f[2], f[3], balance, extra);
                    else if (f[1] == "CHECKING") account = std::make_unique<CheckingAccount>(f[2], f[3], balance, extra);
                    else throw std::runtime_error("unknown account type '" + f[1] + "'");
                    accounts_.push_back(std::move(account));  // unique_ptr is move-only: ownership transfers to the vector

                    // Keep the ID counter ahead of every loaded account number ("ACC1042" -> 1042).
                    nextId_ = std::max(nextId_, std::stoi(f[2].substr(3)) + 1);
                } else if (f[0] == "T") {
                    if (f.size() < 6) throw std::runtime_error("transaction record has too few fields");
                    BankAccount* owner = find(f[1]);
                    if (!owner) throw std::runtime_error("transaction refers to unknown account " + f[1]);
                    owner->restoreTransaction({f[2], f[3], std::stod(f[4]), std::stod(f[5]),
                                               f.size() > 6 ? f[6] : ""});
                } else {
                    throw std::runtime_error("unknown record type '" + f[0] + "'");
                }
            } catch (const std::exception& e) {
                std::cerr << "Warning: skipped line " << lineNo << " of " << filename_ << ": " << e.what() << "\n";
            }
        }
        std::cout << "Loaded " << accounts_.size() << " account(s) from " << filename_ << ".\n";
    }

    // ------------------------------------------------------------------ save
    // ATOMIC SAVE: write everything to a temporary file first, and only
    // replace the real file once the write fully succeeded. If the program
    // crashes or the disk fills up mid-write, the previous good data file is
    // still intact.
    bool save() const {
        const std::string tmp = filename_ + ".tmp";
        {   // inner scope: the ofstream closes (and flushes) at the closing brace (RAII)
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) return false;
            out << "# BankData v1  (A = account, T = transaction; fields separated by '|')\n";
            for (const auto& account : accounts_)  // const auto& - no copy, and unique_ptr could not be copied anyway
                account->serialize(out);           // virtual call: each type writes its own extra field
            out.flush();
            if (!out) return false;
        }
        std::remove(filename_.c_str());  // required on Windows, where rename() will not overwrite
        return std::rename(tmp.c_str(), filename_.c_str()) == 0;
    }

    // ------------------------------------------------------------------ menu
    void run() {
        while (true) {
            printMenu();
            const int choice = input::readNumber<int>("Select an option: ", 0, 7,
                                                      "Please choose a number from 0 to 7.");
            switch (choice) {
                case 1: createAccount();  break;
                case 2: deposit();        break;
                case 3: withdraw();       break;
                case 4: transfer();       break;
                case 5: viewAccount();    break;
                case 6: listAccounts();   break;
                case 7: monthlyUpdate();  break;
                case 0:
                    persist();
                    std::cout << "\nAll data saved. Thank you for banking with us. Goodbye!\n";
                    return;
            }
            input::pause();
        }
    }

private:
    // ------------------------------------------------------------- lookups
    // Returns a NON-OWNING raw pointer (or nullptr if not found).
    // Rule of thumb: unique_ptr = ownership, raw pointer = "just looking".
    // The Bank keeps ownership, so the pointer stays valid while the
    // account is in the vector. We never delete accounts, so it is safe.
    BankAccount* find(const std::string& number) const {
        for (const auto& account : accounts_)
            if (account->number() == number) return account.get();  // .get() exposes the raw pointer without releasing ownership
        return nullptr;
    }

    // Asks for an account number and resolves it; prints an error if unknown.
    BankAccount* promptAccount(const std::string& prompt) const {
        const std::string number = util::toUpper(input::readLine(prompt));
        BankAccount* account = find(number);
        if (!account) std::cout << "  [!] No account found with number '" << number << "'.\n";
        return account;
    }

    // Saves after every state change so a crash can never lose a completed transaction.
    void persist() const {
        if (!save()) std::cerr << "  [!] WARNING: could not write " << filename_ << "! Changes may be lost.\n";
    }

    static void printMenu() {
        std::cout << "\n============================================\n"
                  << "        BANKING MANAGEMENT SYSTEM\n"
                  << "============================================\n"
                  << "  1. Create account\n"
                  << "  2. Deposit\n"
                  << "  3. Withdraw\n"
                  << "  4. Transfer funds\n"
                  << "  5. View account details & history\n"
                  << "  6. List all accounts\n"
                  << "  7. Apply monthly update (interest / fees)\n"
                  << "  0. Exit\n"
                  << "--------------------------------------------\n";
    }

    // ------------------------------------------------------------- actions
    void createAccount() {
        std::cout << "\n--- Create Account ---\n"
                  << "  1. Savings  (" << util::fixed(SavingsAccount::kDefaultRate * 100, 2) << "% APR, minimum balance "
                  << util::money(SavingsAccount::kMinBalance) << ")\n"
                  << "  2. Checking (overdraft limit " << util::money(CheckingAccount::kDefaultOverdraft) << ")\n";
        const int type = input::readNumber<int>("Account type: ", 1, 2, "Please choose 1 or 2.");
        const std::string owner = input::readName("Owner full name: ");

        // Business rule: a savings account must open with at least the minimum balance.
        const double minOpening = (type == 1) ? SavingsAccount::kMinBalance : 0.0;
        const double opening = input::readAmount("Opening deposit: ", minOpening);

        const std::string number = "ACC" + std::to_string(nextId_++);

        // std::make_unique allocates the object and wraps it in a unique_ptr
        // in one exception-safe step. The static type of the variable is the
        // BASE class pointer, but it points at a derived object.
        std::unique_ptr<BankAccount> account;
        if (type == 1) account = std::make_unique<SavingsAccount>(number, owner, 0.0);
        else           account = std::make_unique<CheckingAccount>(number, owner, 0.0);

        // Open with a zero balance and then deposit, so the opening deposit
        // appears in the transaction history like any other deposit.
        std::string error;
        if (opening > 0.0) account->deposit(opening, "Opening deposit", error);

        std::cout << "\n  Account created successfully!\n";
        account->printDetails();
        accounts_.push_back(std::move(account));  // transfer ownership into the bank's vector
        persist();
    }

    void deposit() {
        std::cout << "\n--- Deposit ---\n";
        BankAccount* account = promptAccount("Account number: ");
        if (!account) return;
        const double amount = input::readAmount("Deposit amount: ", 0.01);  // 0.01 minimum rejects zero and negatives
        std::string error;
        if (account->deposit(amount, "Cash deposit", error)) {
            std::cout << "  Deposited " << util::money(amount) << ". New balance: " << util::money(account->balance()) << "\n";
            persist();
        } else {
            std::cout << "  [!] " << error << "\n";
        }
    }

    void withdraw() {
        std::cout << "\n--- Withdraw ---\n";
        BankAccount* account = promptAccount("Account number: ");
        if (!account) return;
        const double amount = input::readAmount("Withdrawal amount: ", 0.01);
        std::string error;
        // Same call for both account types, but different rules run (polymorphism).
        if (account->withdraw(amount, "Cash withdrawal", error)) {
            std::cout << "  Withdrew " << util::money(amount) << ". New balance: " << util::money(account->balance()) << "\n";
            persist();
        } else {
            std::cout << "  [!] " << error << "\n";
        }
    }

    void transfer() {
        std::cout << "\n--- Transfer Funds ---\n";
        BankAccount* from = promptAccount("From account number: ");
        if (!from) return;
        BankAccount* to = promptAccount("To account number  : ");
        if (!to) return;
        if (from == to) { std::cout << "  [!] Cannot transfer to the same account.\n"; return; }

        const double amount = input::readAmount("Transfer amount: ", 0.01);
        std::string error;
        // Order matters: withdraw FIRST. It is the only step that can fail
        // (the source's rules may refuse). The deposit into a valid account
        // with a positive amount cannot fail, so we never end up with money
        // taken but not delivered, and no rollback code is needed.
        if (!from->withdraw(amount, "Transfer to " + to->number(), error)) {
            std::cout << "  [!] Transfer failed: " << error << "\n";
            return;
        }
        to->deposit(amount, "Transfer from " + from->number(), error);
        std::cout << "  Transferred " << util::money(amount) << " from " << from->number() << " to " << to->number() << ".\n"
                  << "  " << from->number() << " balance: " << util::money(from->balance()) << "\n"
                  << "  " << to->number()   << " balance: " << util::money(to->balance()) << "\n";
        persist();
    }

    void viewAccount() const {
        std::cout << "\n--- Account Details ---\n";
        const BankAccount* account = promptAccount("Account number: ");
        if (!account) return;
        std::cout << "\n";
        account->printDetails();     // virtual call: prints the type-specific details
        account->printHistory();
    }

    void listAccounts() const {
        std::cout << "\n--- All Accounts (" << accounts_.size() << ") ---\n";
        if (accounts_.empty()) { std::cout << "  (no accounts yet - create one with option 1)\n"; return; }
        std::cout << "  " << std::left << std::setw(10) << "Number" << std::setw(10) << "Type"
                  << std::setw(26) << "Owner" << std::right << std::setw(16) << "Balance" << "\n"
                  << "  " << std::string(62, '-') << "\n";
        double total = 0.0;
        for (const auto& account : accounts_) {
            std::cout << "  " << std::left << std::setw(10) << account->number()
                      << std::setw(10) << account->typeName()
                      << std::setw(26) << account->owner().substr(0, 24)
                      << std::right << std::setw(16) << util::money(account->balance()) << "\n";
            total += account->balance();
        }
        std::cout << "  " << std::string(62, '-') << "\n"
                  << "  " << std::left << std::setw(46) << "Total holdings" << std::right
                  << std::setw(16) << util::money(total) << "\n" << std::left;
    }

    // Applies interest to savings and fees to checking accounts. The Bank has
    // no idea which kind it is dealing with, and does not need to.
    // That is the payoff of polymorphism.
    void monthlyUpdate() {
        std::cout << "\n--- Monthly Update ---\n";
        if (accounts_.empty()) { std::cout << "  (no accounts)\n"; return; }
        for (auto& account : accounts_) {
            const double before = account->balance();
            account->applyMonthlyUpdate();   // virtual dispatch
            const double delta = account->balance() - before;
            std::cout << "  " << account->number() << " (" << account->typeName() << "): "
                      << (std::fabs(delta) < 0.005 ? std::string("no change") : util::money(delta))
                      << "  -> " << util::money(account->balance()) << "\n";
        }
        persist();
    }

    // ---------------------------------------------------------------- data
    std::string filename_;
    // vector<unique_ptr<Base>> is the standard way to hold a heterogeneous
    // collection of polymorphic objects. A vector<BankAccount> would "slice"
    // derived objects (and would not compile for an abstract class).
    std::vector<std::unique_ptr<BankAccount>> accounts_;
    int nextId_ = 1001;
};

// ============================================================================
//  SECTION 5 - Entry point
// ============================================================================
int main() {
    Bank bank("bank_data.txt");
    try {
        bank.load();
        bank.run();
    } catch (const input::InputClosed&) {
        // stdin closed unexpectedly (Ctrl+D / Ctrl+Z): save and leave cleanly
        // instead of crashing, so no data is lost.
        std::cout << "\nInput closed. Saving data and exiting...\n";
        bank.save();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        bank.save();
        return 1;
    }
    return 0;
}
