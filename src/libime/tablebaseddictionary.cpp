/*
 * Copyright (C) 2015~2017 by CSSlayer
 * wengxt@gmail.com
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; see the file COPYING. If not,
 * see <http://www.gnu.org/licenses/>.
 */

#include "tablebaseddictionary.h"
#include "datrie.h"
#include "lattice.h"
#include "tableoptions.h"
#include "tablerule.h"
#include <boost/algorithm/string.hpp>
#include <cassert>
#include <cstring>
#include <fcitx-utils/utf8.h>
#include <fstream>
#include <set>
#include <string>

namespace libime {

static const char keyValueSeparator = '\x01';

enum {
    STR_KEYCODE,
    STR_CODELEN,
    STR_IGNORECHAR,
    STR_PINYIN,
    STR_PINYINLEN,
    STR_DATA,
    STR_RULE,
    STR_PROMPT,
    STR_CONSTRUCTPHRASE,
    STR_LAST
};

enum class BuildPhase {
    PhaseConfig,
    PhaseRule,
    PhaseData
} phase = BuildPhase::PhaseConfig;

static const char *strConst[2][STR_LAST] = {
    {"键码=", "码长=", "规避字符=", "拼音=", "拼音长度=", "[数据]",
     "[组词规则]", "提示=", "构词="},
    {"KeyCode=", "Length=", "InvalidChar=", "Pinyin=", "PinyinLength=",
     "[Data]", "[Rule]", "Prompt=", "ConstructPhrase="}};

class TableBasedDictionaryPrivate {
public:
    std::vector<TableRule> rules_;
    std::set<char> inputCode_;
    std::set<char> ignoreChars_;
    char pinyinKey_ = '\0';
    char promptKey_ = '\0';
    char phraseKey_ = '\0';
    int32_t codeLength_ = 0;
    int32_t pyLength_ = 0;
    DATrie<float> phraseTrie_;       // base dictionary
    DATrie<float> userTrie_;         // base dictionary
    DATrie<int32_t> singleCharTrie_; // reverse lookup from single character
    DATrie<int32_t> singleCharConstTrie_; // lookup char for new phrase
    DATrie<int32_t> promptTrie_;          // lookup for prompt;
    DATrie<int32_t> pinyinPhraseTrie_;
    std::function<bool(char)> charCheck_;
    TableOptions options_;

    TableBasedDictionaryPrivate() {
        charCheck_ = [this](char c) {
            return inputCode_.find(c) != inputCode_.end();
        };
    }

    TableBasedDictionaryPrivate(const TableBasedDictionaryPrivate &other)
        : rules_(other.rules_), inputCode_(other.inputCode_),
          ignoreChars_(other.ignoreChars_), pinyinKey_(other.pinyinKey_),
          promptKey_(other.promptKey_), phraseKey_(other.phraseKey_),
          codeLength_(other.codeLength_), pyLength_(other.pyLength_),
          phraseTrie_(other.phraseTrie_),
          singleCharTrie_(other.singleCharTrie_),
          singleCharConstTrie_(other.singleCharConstTrie_),
          promptTrie_(other.promptTrie_),
          pinyinPhraseTrie_(other.pinyinPhraseTrie_) {
        charCheck_ = [this](char c) {
            return inputCode_.find(c) != inputCode_.end();
        };
    }

    void reset() {
        pinyinKey_ = promptKey_ = phraseKey_ = '\0';
        codeLength_ = 0;
        pyLength_ = 0;
        inputCode_.clear();
        ignoreChars_.clear();
        rules_.clear();
        rules_.shrink_to_fit();
        phraseTrie_.clear();
        singleCharTrie_.clear();
        singleCharConstTrie_.clear();
        promptTrie_.clear();
        pinyinPhraseTrie_.clear();
    }
    bool validate() {
        if (inputCode_.empty()) {
            return false;
        }
        if (inputCode_.find(pinyinKey_) != inputCode_.end()) {
            return false;
        }
        if (inputCode_.find(promptKey_) != inputCode_.end()) {
            return false;
        }
        if (inputCode_.find(phraseKey_) != inputCode_.end()) {
            return false;
        }
        return true;
    }
};

void updateReverseLookupEntry(DATrie<int32_t> &trie, boost::string_view key,
                              boost::string_view value) {
    auto reverseEntry = value.to_string() + keyValueSeparator;
    bool insert = true;
    trie.foreach (reverseEntry,
                  [&trie, &key, &value, &insert, &reverseEntry](
                      int32_t, size_t len, DATrie<int32_t>::position_type pos) {
                      auto oldKeyLen = len;
                      if (key.length() > oldKeyLen) {
                          std::string oldEntry;
                          trie.suffix(oldEntry, len, pos);
                          trie.erase(pos);
                      } else {
                          insert = false;
                      }
                      return false;
                  });
    if (insert) {
        reverseEntry.append(key.begin(), key.end());
        trie.set(reverseEntry, 1);
    }
}

TableBasedDictionary::TableBasedDictionary()
    : d_ptr(std::make_unique<TableBasedDictionaryPrivate>()) {
    FCITX_D();
    d->reset();
}

TableBasedDictionary::~TableBasedDictionary() {}

TableBasedDictionary::TableBasedDictionary(
    TableBasedDictionary &&other) noexcept
    : d_ptr(std::move(other.d_ptr)) {}

TableBasedDictionary::TableBasedDictionary(
    const libime::TableBasedDictionary &other)
    : d_ptr(std::make_unique<TableBasedDictionaryPrivate>(*other.d_ptr)) {}

TableBasedDictionary &TableBasedDictionary::
operator=(TableBasedDictionary other) {
    swap(*this, other);
    return *this;
}

void TableBasedDictionary::load(const char *filename, TableFormat format) {
    std::ifstream in(filename, std::ios::in | std::ios::binary);
    throw_if_io_fail(in);
    load(in, format);
}

void TableBasedDictionary::load(std::istream &in, TableFormat format) {
    switch (format) {
    case TableFormat::Binary:
        loadBinary(in);
        break;
    case TableFormat::Text:
        loadText(in);
        break;
    default:
        throw std::invalid_argument("unknown format type");
    }
}

void TableBasedDictionary::parseDataLine(const std::string &buf) {
    FCITX_D();
    char special[3] = {d->pinyinKey_, d->phraseKey_, d->promptKey_};
    PhraseFlag specialFlag[] = {PhraseFlagPinyin, PhraseFlagConstructPhrase,
                                PhraseFlagPrompt};
    auto spacePos = buf.find_first_of(" \n\r\f\v\t");
    if (spacePos == std::string::npos || spacePos + 1 == buf.size()) {
        return;
    }
    auto wordPos = buf.find_first_not_of(" \n\r\f\v\t", spacePos);
    if (spacePos == std::string::npos || spacePos + 1 == buf.size()) {
        return;
    }

    auto key = boost::string_view(buf).substr(0, spacePos);
    auto value = boost::string_view(buf).substr(wordPos);

    auto iter = std::find(std::begin(special), std::end(special), key[0]);
    PhraseFlag flag = PhraseFlagNone;
    if (iter != std::end(special)) {
        flag = specialFlag[iter - std::begin(special)];
        key = key.substr(1);
    }

    insert(key.to_string(), value.to_string(), flag);
}

void TableBasedDictionary::loadText(std::istream &in) {
    FCITX_D();
    d->reset();

    std::string buf;
    size_t lineNumber = 0;

    auto check_option = [&buf](int index) {
        if (buf.compare(0, std::strlen(strConst[0][index]),
                        strConst[0][index]) == 0) {
            return 0;
        } else if (buf.compare(0, std::strlen(strConst[1][index]),
                               strConst[1][index]) == 0) {
            return 1;
        }
        return -1;
    };

    auto isSpaceCheck = boost::is_any_of(" \n\t\r\v\f");
    while (!in.eof()) {
        if (!std::getline(in, buf)) {
            break;
        }
        lineNumber++;

        boost::trim_if(buf, isSpaceCheck);

        switch (phase) {
        case BuildPhase::PhaseConfig: {
            if (buf[0] == '#') {
                continue;
            }

            int match = -1;
            if ((match = check_option(STR_KEYCODE)) >= 0) {
                const std::string code =
                    buf.substr(strlen(strConst[match][STR_KEYCODE]));
                d->inputCode_ = std::set<char>(code.begin(), code.end());
            } else if ((match = check_option(STR_CODELEN)) >= 0) {
                d->codeLength_ =
                    std::stoi(buf.substr(strlen(strConst[match][STR_CODELEN])));
            } else if ((match = check_option(STR_PINYINLEN)) >= 0) {
                d->pyLength_ = std::stoi(
                    buf.substr(strlen(strConst[match][STR_PINYINLEN])));
            } else if ((match = check_option(STR_IGNORECHAR)) >= 0) {
                const std::string ignoreChars =
                    buf.substr(strlen(strConst[match][STR_KEYCODE]));
                d->ignoreChars_ =
                    std::set<char>(ignoreChars.begin(), ignoreChars.end());
            } else if ((match = check_option(STR_PINYIN)) >= 0) {
                d->pinyinKey_ = buf[strlen(strConst[match][STR_PINYIN])];
            } else if ((match = check_option(STR_PROMPT)) >= 0) {
                d->promptKey_ = buf[strlen(strConst[match][STR_PROMPT])];
            } else if ((match = check_option(STR_CONSTRUCTPHRASE)) >= 0) {
                d->phraseKey_ =
                    buf[strlen(strConst[match][STR_CONSTRUCTPHRASE])];
            } else if (check_option(STR_DATA) >= 0) {
                phase = BuildPhase::PhaseData;
                if (!d->validate()) {
                    throw std::invalid_argument("file format is invalid");
                }
                break;
            } else if (check_option(STR_RULE) >= 0) {
                phase = BuildPhase::PhaseRule;
                break;
            }
            break;
        }
        case BuildPhase::PhaseRule: {
            if (buf[0] == '#') {
                continue;
            }
            if (check_option(STR_DATA) >= 0) {
                phase = BuildPhase::PhaseData;
                if (!d->validate()) {
                    throw std::invalid_argument("file format is invalid");
                }
                break;
            }

            if (buf.empty()) {
                continue;
            }

            d->rules_.emplace_back(buf, d->codeLength_);
        }
        case BuildPhase::PhaseData:
            parseDataLine(buf);
            break;
        }
    }

    if (phase != BuildPhase::PhaseData) {
        throw_if_fail(in.bad(), std::ios_base::failure("io failed"));
        throw std::invalid_argument("file format is invalid");
    }
}

void TableBasedDictionary::saveText(std::ostream &out) {
    FCITX_D();
    out << strConst[1][STR_KEYCODE];
    for (auto c : d->inputCode_) {
        out << c;
    }
    out << std::endl;
    out << strConst[1][STR_CODELEN] << d->codeLength_ << std::endl;
    if (d->ignoreChars_.size()) {
        out << strConst[1][STR_IGNORECHAR];
        for (auto c : d->ignoreChars_) {
            out << c;
        }
        out << std::endl;
    }
    if (d->pinyinKey_) {
        out << strConst[1][STR_PINYIN] << d->pinyinKey_ << std::endl;
        out << strConst[1][STR_PINYINLEN] << d->pyLength_ << std::endl;
    }
    if (d->promptKey_) {
        out << strConst[1][STR_PROMPT] << d->promptKey_ << std::endl;
    }
    if (d->phraseKey_) {
        out << strConst[1][STR_CONSTRUCTPHRASE] << d->phraseKey_ << std::endl;
    }

    if (hasRule()) {
        out << strConst[1][STR_RULE] << std::endl;
        for (const auto &rule : d->rules_) {
            out << rule.toString() << std::endl;
        }
    }
    out << strConst[1][STR_DATA] << std::endl;
    std::string buf;
    if (d->promptKey_) {
        d->promptTrie_.foreach ([this, d, &buf, &out](
            int32_t, size_t _len, DATrie<int32_t>::position_type pos) {
            d->promptTrie_.suffix(buf, _len, pos);
            auto sep = buf.find(keyValueSeparator);
            boost::string_view ref(buf);
            out << d->promptKey_ << ref.substr(0, sep) << " "
                << ref.substr(sep + 1) << std::endl;
            return true;
        });
    }
    if (d->phraseKey_) {
        d->singleCharConstTrie_.foreach ([this, d, &buf, &out](
            int32_t, size_t _len, DATrie<int32_t>::position_type pos) {
            d->singleCharConstTrie_.suffix(buf, _len, pos);
            auto sep = buf.find(keyValueSeparator);
            boost::string_view ref(buf);
            out << d->promptKey_ << ref.substr(sep + 1) << " "
                << ref.substr(0, sep) << std::endl;
            return true;
        });
    }
    d->phraseTrie_.foreach ([this, d, &buf, &out](
        int32_t, size_t _len, DATrie<int32_t>::position_type pos) {
        d->phraseTrie_.suffix(buf, _len, pos);
        auto sep = buf.find(keyValueSeparator);
        boost::string_view ref(buf);
        out << ref.substr(0, sep) << " " << ref.substr(sep + 1) << std::endl;
        return true;
    });
    d->pinyinPhraseTrie_.foreach ([this, d, &buf, &out](
        int32_t, size_t _len, DATrie<int32_t>::position_type pos) {
        d->pinyinPhraseTrie_.suffix(buf, _len, pos);
        auto sep = buf.find(keyValueSeparator);
        boost::string_view ref(buf);
        out << d->pinyinKey_ << ref.substr(0, sep) << " " << ref.substr(sep + 1)
            << std::endl;
        return true;
    });
}

void TableBasedDictionary::loadBinary(std::istream &in) {
    FCITX_D();
    throw_if_io_fail(unmarshall(in, d->pinyinKey_));
    throw_if_io_fail(unmarshall(in, d->promptKey_));
    throw_if_io_fail(unmarshall(in, d->phraseKey_));
    throw_if_io_fail(unmarshall(in, d->codeLength_));
    throw_if_io_fail(unmarshall(in, d->pyLength_));
    uint32_t size;

    throw_if_io_fail(unmarshall(in, size));
    d->inputCode_.clear();
    while (size--) {
        char c;
        throw_if_io_fail(unmarshall(in, c));
        d->inputCode_.insert(c);
    }

    throw_if_io_fail(unmarshall(in, size));
    d->ignoreChars_.clear();
    while (size--) {
        char c;
        throw_if_io_fail(unmarshall(in, c));
        d->ignoreChars_.insert(c);
    }

    throw_if_io_fail(unmarshall(in, size));
    d->rules_.clear();
    while (size--) {
        d->rules_.emplace_back(in);
    }
    d->phraseTrie_ = decltype(d->phraseTrie_)(in);
    d->singleCharTrie_ = decltype(d->singleCharTrie_)(in);
    if (hasRule()) {
        d->singleCharConstTrie_ = decltype(d->singleCharConstTrie_)(in);
    }
    if (d->promptKey_) {
        d->promptTrie_ = decltype(d->promptTrie_)(in);
    }
    if (d->pinyinKey_) {
        d->pinyinPhraseTrie_ = decltype(d->pinyinPhraseTrie_)(in);
    }
}

void TableBasedDictionary::save(const char *filename, TableFormat format) {
    std::ofstream fout(filename, std::ios::out | std::ios::binary);
    throw_if_io_fail(fout);
    save(fout, format);
}

void TableBasedDictionary::save(std::ostream &out, TableFormat format) {
    switch (format) {
    case TableFormat::Binary:
        saveBinary(out);
        break;
    case TableFormat::Text:
        saveText(out);
        break;
    default:
        throw std::invalid_argument("unknown format type");
    }
}

void TableBasedDictionary::saveBinary(std::ostream &out) {
    FCITX_D();
    throw_if_io_fail(marshall(out, d->pinyinKey_));
    throw_if_io_fail(marshall(out, d->promptKey_));
    throw_if_io_fail(marshall(out, d->phraseKey_));
    throw_if_io_fail(marshall(out, d->codeLength_));
    throw_if_io_fail(marshall(out, d->pyLength_));
    throw_if_io_fail(
        marshall(out, static_cast<uint32_t>(d->inputCode_.size())));
    for (auto c : d->inputCode_) {
        throw_if_io_fail(marshall(out, c));
    }
    throw_if_io_fail(
        marshall(out, static_cast<uint32_t>(d->ignoreChars_.size())));
    for (auto c : d->ignoreChars_) {
        throw_if_io_fail(marshall(out, c));
    }
    throw_if_io_fail(marshall(out, static_cast<uint32_t>(d->rules_.size())));
    for (const auto &rule : d->rules_) {
        throw_if_io_fail(out << rule);
    }
    d->phraseTrie_.save(out);
    d->singleCharTrie_.save(out);
    if (hasRule()) {
        d->singleCharConstTrie_.save(out);
    }
    if (d->promptKey_) {
        d->promptTrie_.save(out);
    }
    if (d->pinyinKey_) {
        d->pinyinPhraseTrie_.save(out);
    }
}

void TableBasedDictionary::loadUser(const char *filename, TableFormat format) {
    std::ifstream in(filename, std::ios::in | std::ios::binary);
    throw_if_io_fail(in);
    loadUser(in, format);
}

void TableBasedDictionary::loadUser(std::istream &in, TableFormat format) {
    FCITX_D();
    switch (format) {
    case TableFormat::Binary:
        d->userTrie_ = decltype(d->userTrie_)(in);
        break;
    case TableFormat::Text: {
        std::string buf;
        while (!in.eof()) {
            if (!std::getline(in, buf)) {
                break;
            }

            parseDataLine(buf);
        }
        break;
    }
    default:
        throw std::invalid_argument("unknown format type");
    }
}

void TableBasedDictionary::saveUser(const char *filename, TableFormat) {
    std::ofstream fout(filename, std::ios::out | std::ios::binary);
    throw_if_io_fail(fout);
    save(fout);
}

void TableBasedDictionary::saveUser(std::ostream &out, TableFormat format) {
    FCITX_D();
    switch (format) {
    case TableFormat::Binary:
        d->userTrie_.save(out);
        throw_if_io_fail(out);
        break;
    case TableFormat::Text: {
        std::string buf;
        d->phraseTrie_.foreach ([this, d, &buf, &out](
            int32_t, size_t _len, DATrie<int32_t>::position_type pos) {
            d->phraseTrie_.suffix(buf, _len, pos);
            auto sep = buf.find(keyValueSeparator);
            boost::string_view ref(buf);
            out << ref.substr(0, sep) << " " << ref.substr(sep + 1)
                << std::endl;
            return true;
        });
        break;
    }
    default:
        throw std::invalid_argument("unknown format type");
    }
}

bool TableBasedDictionary::hasRule() const noexcept {
    FCITX_D();
    return !d->rules_.empty();
}

bool TableBasedDictionary::insert(const std::string &value) {
    std::string key;
    if (generate(value, key)) {
        return insert(key, value);
    }
    return false;
}

bool TableBasedDictionary::insert(const std::string &key,
                                  const std::string &value, PhraseFlag flag,
                                  bool verifyWithRule) {
    FCITX_D();
    if (flag != PhraseFlagPinyin && !boost::all(key, d->charCheck_)) {
        return false;
    }

    // invalid char
    if (!fcitx::utf8::validate(value.c_str())) {
        return false;
    }

    switch (flag) {
    case PhraseFlagNone: {
        if (verifyWithRule && hasRule()) {
            std::string checkKey;
            if (!generate(value, checkKey)) {
                return false;
            }
            if (checkKey != key) {
                return false;
            }
        }

        auto entry = key + keyValueSeparator + value;
        if (d->phraseTrie_.exactMatchSearch(entry) > 0) {
            return false;
        }
        d->phraseTrie_.set(entry, flag);

        if (fcitx::utf8::length(value) == 1) {
            updateReverseLookupEntry(d->singleCharTrie_, key, value);

            if (hasRule() && !d->phraseKey_) {
                updateReverseLookupEntry(d->singleCharConstTrie_, key, value);
            }
        }
        break;
    }
    case PhraseFlagPinyin: {
        auto entry = key + keyValueSeparator + value;
        if (d->pinyinPhraseTrie_.exactMatchSearch(entry) > 0) {
            return false;
        }
        d->pinyinPhraseTrie_.set(entry, flag);
        break;
    }
    case PhraseFlagPrompt:
        if (key.size() == 1) {
            auto promptChar = fcitx::utf8::getCharValidated(value);
            d->promptTrie_.set(key, promptChar);
        } else {
            return false;
        }
        break;
    case PhraseFlagConstructPhrase:
        if (hasRule()) {
            updateReverseLookupEntry(d->singleCharConstTrie_, key, value);
        }
        break;
    }
    return true;
}

bool TableBasedDictionary::generate(const std::string &value,
                                    std::string &key) {
    FCITX_D();
    if (!hasRule() || value.empty()) {
        return false;
    }

    if (!fcitx::utf8::validate(value)) {
        return false;
    }

    auto valueLen = fcitx::utf8::length(value);

    std::string newKey;
    std::string entry;
    for (const auto &rule : d->rules_) {
        // check rule can be applied
        if (!((rule.flag == TableRuleFlag::LengthEqual &&
               valueLen == rule.phraseLength) ||
              (rule.flag == TableRuleFlag::LengthLongerThan &&
               valueLen >= rule.phraseLength))) {
            continue;
        }

        bool success = true;
        for (const auto &ruleEntry : rule.entries) {
            std::string::const_iterator iter;
            if (ruleEntry.character == 0 && ruleEntry.encodingIndex == 0) {
                continue;
            }

            if (ruleEntry.flag == TableRuleEntryFlag::FromFront) {
                if (ruleEntry.character > valueLen) {
                    success = false;
                    break;
                }

                iter = value.begin() +
                       fcitx::utf8::nthChar(value, ruleEntry.character - 1);
            } else {
                if (ruleEntry.character > valueLen) {
                    success = false;
                    break;
                }

                iter =
                    value.begin() +
                    fcitx::utf8::nthChar(value, valueLen - ruleEntry.character);
            }
            auto prev = iter;
            iter = value.begin() +
                   fcitx::utf8::nthChar(value, iter - value.begin(), 1);
            std::string s(prev, iter);
            s += keyValueSeparator;

            size_t len = 0;
            d->singleCharConstTrie_.foreach (
                s, [this, d, &len, &entry](int32_t, size_t _len,
                                           DATrie<int32_t>::position_type pos) {
                    len = _len;
                    d->singleCharConstTrie_.suffix(entry, _len, pos);
                    return false;
                });

            if (!len) {
                success = false;
                break;
            }

            auto key = entry;
            if (key.size() < ruleEntry.encodingIndex) {
                continue;
            }

            newKey.append(1, key[ruleEntry.encodingIndex - 1]);
        }

        if (success && !newKey.empty()) {
            key = newKey;
            return true;
        }
    }

    return false;
}

bool TableBasedDictionary::isInputCode(uint32_t c) const {
    FCITX_D();
    return !!(d->inputCode_.count(c));
}

void TableBasedDictionary::statistic() const {
    FCITX_D();
    std::cout << "Phrase Trie: " << d->phraseTrie_.mem_size() << std::endl
              << "Single Char Trie: " << d->singleCharTrie_.mem_size()
              << std::endl
              << "Single char const trie: "
              << d->singleCharConstTrie_.mem_size() << std::endl
              << "Prompt Trie: " << d->promptTrie_.mem_size() << std::endl
              << "pinyin phrase trie: " << d->pinyinPhraseTrie_.mem_size()
              << std::endl;
}

void TableBasedDictionary::setTableOptions(TableOptions option) {
    FCITX_D();
    d->options_ = option;
}

const TableOptions &TableBasedDictionary::tableOptions() const {
    FCITX_D();
    return d->options_;
}

bool TableBasedDictionary::hasPinyin() const {
    FCITX_D();
    return d->pinyinPhraseTrie_.size();
}

int32_t TableBasedDictionary::inputLength() const {
    FCITX_D();
    return d->codeLength_;
}

int32_t TableBasedDictionary::pinyinLength() const {
    FCITX_D();
    return d->pyLength_;
}

void TableBasedDictionary::matchWords(
    boost::string_view code, TableMatchMode mode,
    const TableMatchCallback &callback) const {
    FCITX_D();
    d->phraseTrie_.foreach (
        code.data(), code.size(),
        [this, d, &code, callback, mode](float cost, size_t len,
                                         DATrie<int32_t>::position_type pos) {
            std::string entry;
            d->phraseTrie_.suffix(entry, code.size() + len, pos);
            auto sep = entry.find(keyValueSeparator, code.size());
            if (sep == std::string::npos) {
                return true;
            }
            if (mode == TableMatchMode::Prefix ||
                (mode == TableMatchMode::Exact && sep == code.size())) {
                auto view = boost::string_view(entry);
                return callback(view.substr(0, sep), view.substr(sep + 1),
                                cost);
            }
            return true;
        });
}

void TableBasedDictionary::matchPrefixImpl(
    const SegmentGraph &graph, const GraphMatchCallback &callback,
    const std::unordered_set<const SegmentGraphNode *> &ignore, void *) const {
    assert(graph.isList());
    TableMatchMode mode = tableOptions().exactMatch() ? TableMatchMode::Exact
                                                      : TableMatchMode::Prefix;
    SegmentGraphPath path;
    path.reserve(2);
    for (auto *node = &graph.start(); node;
         node = node->nextSize() ? &node->nexts().front() : nullptr) {
        if (!node->prevSize() || ignore.count(node)) {
            continue;
        }
        path.clear();
        path.push_back(&node->prevs().front());
        path.push_back(node);

        auto code = graph.segment(*path[0], *path[1]);
        bool matched = false;
        matchWords(code, mode, [&](boost::string_view code,
                                   boost::string_view word, float score) {
            WordNode wordNode(word, InvalidWordIndex);
            callback(path, wordNode, score, code);
            matched = true;
            return true;
        });
        if (!matched) {
            WordNode wordNode("", 0);
            callback(path, wordNode, 0, code);
        }
    }
}

void swap(TableBasedDictionary &lhs, TableBasedDictionary &rhs) noexcept {
    using std::swap;
    swap(lhs.d_ptr, rhs.d_ptr);
}
}
