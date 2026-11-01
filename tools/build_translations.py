#!/usr/bin/env python3
# NixSCP translations: joins WinSCP's translations
# (winscp-master/translations/<CODE>.ini, keyed by form component paths and
# resource string identifiers) with WinSCP's English originals
# (source/forms/*.dfm: Caption, Hint, Text, Items.Strings, collection item
# captions; source/resource/*.rc: string tables) and writes
#
#   resources/translations/<code>.json
#     { "lang": "SRL", "name": "srpski (latinica)", "english": "Serbian (Latin)",
#       "locale": "sr-Latn", "lcid": 2074, "completeness": 97, "rtl": false,
#       "authors": "...",
#       "strings": { "<English>": "<localized>", ... },          most frequent translation
#       "contexts": { "<FormName>": { "<English>": "<localized>" } } }   per-form deviations
#   resources/translations/index.json   (languages without their strings)
#   resources/translations.qrc
#
# Texts keep their "&" mnemonics; mnemonic-free variants (English without "&"
# -> translation without "&" / "(&X)") are recorded too, unless they would
# collide with a real text. Line endings are normalized to "\n".
#
# The translations are GNU GPL (as WinSCP); the generated JSON is committed so
# that builds do not need the WinSCP tree.
#
# Usage: python3 -I tools/build_translations.py --winscp <winscp-master> [--root <nixscp>]
# Uses the Python standard library only.

import argparse
import collections
import json
import os
import re
import sys

# WinSCP translation code (LOCALE_SABBREVLANGNAME based file extension) ->
# (BCP-47 locale, Windows LCID as WinSCP stores it in LocaleSafe, English name, native name).
# Keep in sync with Languages[] in src/gui/Translator.cpp.
LANGUAGES = {
    'AF': ('af', 0x0436, 'Afrikaans', 'Afrikaans'),
    'AR': ('ar', 0x0401, 'Arabic', 'العربية'),
    'BE': ('be', 0x0423, 'Belarusian', 'беларуская'),
    'BG': ('bg', 0x0402, 'Bulgarian', 'български'),
    'BN': ('bn', 0x0445, 'Bangla', 'বাংলা'),
    'CA': ('ca', 0x0403, 'Catalan', 'català'),
    'CH': ('zh-Hant', 0x0404, 'Chinese (Traditional)', '中文(繁體)'),
    'CHS': ('zh-Hans', 0x0804, 'Chinese (Simplified)', '中文(简体)'),
    'CS': ('cs', 0x0405, 'Czech', 'čeština'),
    'DA': ('da', 0x0406, 'Danish', 'dansk'),
    'DE': ('de', 0x0407, 'German', 'Deutsch'),
    'EL': ('el', 0x0408, 'Greek', 'Ελληνικά'),
    'ES': ('es', 0x0C0A, 'Spanish', 'español'),
    'ET': ('et', 0x0425, 'Estonian', 'eesti'),
    'FA': ('fa', 0x0429, 'Persian', 'فارسی'),
    'FI': ('fi', 0x040B, 'Finnish', 'suomi'),
    'FP': ('fil', 0x0464, 'Filipino', 'Filipino'),
    'FR': ('fr', 0x040C, 'French', 'français'),
    'GL': ('gl', 0x0456, 'Galician', 'galego'),
    'HE': ('he', 0x040D, 'Hebrew', 'עברית'),
    'HI': ('hi', 0x0439, 'Hindi', 'हिन्दी'),
    'HR': ('hr', 0x041A, 'Croatian', 'hrvatski'),
    'HU': ('hu', 0x040E, 'Hungarian', 'magyar'),
    'HY': ('hy', 0x042B, 'Armenian', 'հայերեն'),
    'IN': ('id', 0x0421, 'Indonesian', 'Indonesia'),
    'IS': ('is', 0x040F, 'Icelandic', 'íslenska'),
    'IT': ('it', 0x0410, 'Italian', 'italiano'),
    'JP': ('ja', 0x0411, 'Japanese', '日本語'),
    'KA': ('ka', 0x0437, 'Georgian', 'ქართული'),
    'KO': ('ko', 0x0412, 'Korean', '한국어'),
    'LT': ('lt', 0x0427, 'Lithuanian', 'lietuvių'),
    'LV': ('lv', 0x0426, 'Latvian', 'latviešu'),
    'MK': ('mk', 0x042F, 'Macedonian', 'македонски'),
    'MS': ('ms', 0x043E, 'Malay', 'Melayu'),
    'MYA': ('my', 0x0455, 'Burmese', 'မြန်မာ'),
    'NL': ('nl', 0x0413, 'Dutch', 'Nederlands'),
    'NO': ('nb', 0x0414, 'Norwegian Bokmål', 'norsk bokmål'),
    'PA': ('pa', 0x0446, 'Punjabi', 'ਪੰਜਾਬੀ'),
    'PL': ('pl', 0x0415, 'Polish', 'polski'),
    'PT': ('pt-BR', 0x0416, 'Portuguese (Brazil)', 'português (Brasil)'),
    'PTG': ('pt-PT', 0x0816, 'Portuguese (Portugal)', 'português (Portugal)'),
    'RO': ('ro', 0x0418, 'Romanian', 'română'),
    'RU': ('ru', 0x0419, 'Russian', 'русский'),
    'SK': ('sk', 0x041B, 'Slovak', 'slovenčina'),
    'SL': ('sl', 0x0424, 'Slovenian', 'slovenščina'),
    'SQ': ('sq', 0x041C, 'Albanian', 'shqip'),
    'SRL': ('sr-Latn', 0x081A, 'Serbian (Latin)', 'srpski (latinica)'),
    'SV': ('sv', 0x041D, 'Swedish', 'svenska'),
    'TA': ('ta', 0x0449, 'Tamil', 'தமிழ்'),
    'TH': ('th', 0x041E, 'Thai', 'ไทย'),
    'TR': ('tr', 0x041F, 'Turkish', 'Türkçe'),
    'UK': ('uk', 0x0422, 'Ukrainian', 'українська'),
    'VI': ('vi', 0x042A, 'Vietnamese', 'Tiếng Việt'),
    # AdditionalLanguageMask (0xFFFFFF00) + 'E' / 'K'
    'XXE': ('eo', 0xFFFFFF45, 'Esperanto', 'esperanto'),
    'XXK': ('kab', 0xFFFFFF4B, 'Kabyle', 'Taqbaylit'),
}

RTL = {'AR', 'HE', 'FA'}

PROPERTIES = ('Caption', 'Hint', 'Text', 'DialogText', 'DialogTitle', 'Title', 'ButtonHint')

# Sections of the translation files that are not WinSCP's user interface
SKIPPED_SECTIONS = ('default.isl', 'winscp.isl', 'Propagation.rc', 'Extensions.rc')
SKIPPED_KEYS = re.compile(r'^(TRANSLATOR_|TRANSLATION_COMPLETENESS|BIDI_MODE|FLIP_CHILDREN|WIN_ERROR_STRINGS|'
                          r'DESCRIPTION_|FUNCTIONS_)')

# English texts of the Delphi resource strings (WinSCP_DRC.rc; not in the WinSCP tree)
DRC = {
    'Baseutils_SNoValidPath': "Can't find any valid path.",
    'Comboedit_SBrowse': 'Browse',
    'Comboedit_SDefaultFilter': 'All files (*.*)|*.*',
    'Comboedit_SInvalidFileName': 'Invalid file name - %s',
    'Customdirview_SDirNotExists': "Directory '%s' doesn't exist.",
    'Customdirview_SDriveNotReady': "Drive '%s:' is not ready.",
    'Customdirview_SErrorInvalidName': 'Filename contains invalid characters:',
    'Customdirview_SErrorRenameFile': "Can't rename file or directory:",
    'Customdirview_SErrorRenameFileExists': 'File already exists:',
    'Customdirview_SParentDir': 'Parent directory',
    'Customdirview_STextFileExt': 'File %s',
    'Customunixdirview_SUnixDefaultRootName': '/ <root>',
    'Dirviewcolproperties_SDirViewAttrCol': 'Attr',
    'Dirviewcolproperties_SDirViewChangedCol2': 'Date modified',
    'Dirviewcolproperties_SDirViewExtCol': 'Ext',
    'Dirviewcolproperties_SDirViewNameCol': 'Name',
    'Dirviewcolproperties_SDirViewSizeCol': 'Size',
    'Dirviewcolproperties_SDirViewTypeCol': 'Type',
    'Dragdrop_MIAbortStr': '&Cancel',
    'Dragdrop_MICopyStr': '&Copy Here',
    'Dragdrop_MILinkStr': 'Create &Shortcut(s) Here',
    'Dragdrop_MIMoveStr': '&Move Here',
    'Unixdirviewcolproperties_SUnixDirViewGroupCol': 'Group',
    'Unixdirviewcolproperties_SUnixDirViewLinkTargetCol': 'Link target',
    'Unixdirviewcolproperties_SUnixDirViewOwnerCol': 'Owner',
    'Unixdirviewcolproperties_SUnixDirViewRightsCol': 'Rights',
    'Unixdirviewcolproperties_SUnixDirViewTypeCol': 'Type',
    'Vcl_Consts_SCancelButton': 'Cancel',
    'Vcl_Consts_SMsgDlgAbort': '&Abort',
    'Vcl_Consts_SMsgDlgAll': '&All',
    'Vcl_Consts_SMsgDlgCancel': 'Cancel',
    'Vcl_Consts_SMsgDlgClose': '&Close',
    'Vcl_Consts_SMsgDlgConfirm': 'Confirm',
    'Vcl_Consts_SMsgDlgError': 'Error',
    'Vcl_Consts_SMsgDlgHelp': '&Help',
    'Vcl_Consts_SMsgDlgIgnore': '&Ignore',
    'Vcl_Consts_SMsgDlgInformation': 'Information',
    'Vcl_Consts_SMsgDlgNo': '&No',
    'Vcl_Consts_SMsgDlgNoToAll': 'N&o to All',
    'Vcl_Consts_SMsgDlgOK': 'OK',
    'Vcl_Consts_SMsgDlgRetry': '&Retry',
    'Vcl_Consts_SMsgDlgWarning': 'Warning',
    'Vcl_Consts_SMsgDlgYes': '&Yes',
    'Vcl_Consts_SMsgDlgYesToAll': 'Yes to &All',
    'Vcl_Consts_SOKButton': 'OK',
}


# ---------------------------------------------------------------- .dfm parsing
def dfm_string(expr):
    """Delphi string expression: 'abc' + 'def'#13#10'x' -> python str"""
    out = []
    i = 0
    n = len(expr)
    while i < n:
        c = expr[i]
        if c == "'":
            i += 1
            while i < n:
                if expr[i] == "'":
                    if i + 1 < n and expr[i + 1] == "'":
                        out.append("'")
                        i += 2
                        continue
                    i += 1
                    break
                out.append(expr[i])
                i += 1
        elif c == '#':
            m = re.match(r'#(\$[0-9A-Fa-f]+|\d+)', expr[i:])
            if not m:
                break
            v = m.group(1)
            out.append(chr(int(v[1:], 16) if v.startswith('$') else int(v)))
            i += len(m.group(0))
        else:
            i += 1   # '+', whitespace
    return ''.join(out)


def parse_dfm(path):
    """Yields (key, English text) with keys as in the WinSCP translation files"""
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')
    stack = []          # component names (root excluded from keys)
    collection = []     # [key prefix, item index, inside item] while inside "Prop = <"
    result = []
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        i += 1
        m = re.match(r'^(object|inherited|inline)\s+(?:(\w+)\s*:\s*)?[\w.]+(\s*\[\d+\])?$', line)
        if m:
            # anonymous components ("object TTBXItem") cannot be translated
            stack.append(m.group(2) or '?')
            continue
        if line == 'end' or line == 'end>':
            if collection and collection[-1][2]:
                collection[-1][2] = False   # end of collection item
                if line == 'end>':
                    collection.pop()
            elif stack:
                stack.pop()
            continue
        if collection and line == 'item':
            collection[-1][1] += 1
            collection[-1][2] = True
            continue
        m = re.match(r'^([\w.]+)\s*=\s*(.*)$', line)
        if not m:
            continue
        prop, value = m.group(1), m.group(2)
        path_names = stack[1:]
        if collection and collection[-1][2]:
            prefix = collection[-1][0] + ['item%d' % collection[-1][1]]
        else:
            prefix = path_names
        if value == '<':
            collection.append([path_names, -1, False])
            continue
        if value.startswith('<') and value.endswith('>'):
            continue
        if value == '(':
            # Items.Strings = ( 'a' 'b' )
            items = []
            cur = ''
            while i < len(lines):
                l = lines[i].strip()
                i += 1
                end = l.endswith(')')
                if end:
                    l = l[:-1]
                if l.endswith('+'):
                    cur += l[:-1]
                else:
                    cur += l
                    if cur.strip():
                        items.append(dfm_string(cur))
                    cur = ''
                if end:
                    break
            if prop.endswith('Strings') and '?' not in prefix:
                for k, t in enumerate(items):
                    result.append(('.'.join(prefix + [prop, str(k)]), t))
            continue
        # multi-line string: "Caption = " followed by continuation lines ending with '+'
        expr = value
        if expr == '':
            while i < len(lines):
                l = lines[i].strip()
                i += 1
                expr += l
                if not l.endswith('+'):
                    break
        else:
            while expr.rstrip().endswith('+') and i < len(lines):
                expr += lines[i].strip()
                i += 1
        if '?' in prefix:
            continue
        if prop.split('.')[-1] in PROPERTIES and (expr.startswith("'") or expr.startswith('#')):
            result.append(('.'.join(prefix + [prop]), dfm_string(expr)))
    return result


# ---------------------------------------------------------------- .rc parsing
def c_unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            d = s[i + 1]
            out.append({'n': '\n', 'r': '\r', 't': '\t', '\\': '\\', '"': '"', "'": "'"}.get(d, d))
            i += 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def parse_rc(path):
    result = []
    pending = None   # "ID," followed by "..." continuation lines
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if pending is not None:
                m = re.match(r'^\s*"(.*)"\s*$', line)
                if m:
                    pending[1].append(m.group(1))
                    continue
                result.append((pending[0], c_unescape(''.join(pending[1]).replace('""', '\\"'))))
                pending = None
            m = re.match(r'^\s*(\w+)\s*,\s*"(.*)"\s*(//.*)?$', line)
            if m:
                result.append((m.group(1), c_unescape(m.group(2).replace('""', '\\"'))))
                continue
            m = re.match(r'^\s*(\w+)\s*,\s*$', line)
            if m:
                pending = (m.group(1), [])
    return result


# ---------------------------------------------------------------- translations
def parse_translation(path):
    sections = {}
    current = None
    with open(path, encoding='utf-8-sig', errors='replace') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if not line or line.startswith(';'):
                continue
            if line.startswith('[') and line.endswith(']'):
                current = sections.setdefault(line[1:-1], {})
                continue
            if current is None or '=' not in line:
                continue
            key, value = line.split('=', 1)
            value = value.strip()
            if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
                value = value[1:-1]
            current[key.strip()] = c_unescape(value)
    return sections


def normalize_text(s):
    return s.replace('\r\n', '\n').replace('\r', '\n')


MNEMONIC_SUFFIX = re.compile(r'\s*\(&[^&\s]\)')   # "Datei(&F)" (CJK style)


def strip_mnemonic(s):
    s = MNEMONIC_SUFFIX.sub('', s)
    return s.replace('&&', '\x00').replace('&', '').replace('\x00', '&')


def has_mnemonic(s):
    return '&' in s.replace('&&', '')


def has_letters(s):
    return any(ch.isalpha() for ch in s)


# ---------------------------------------------------------------- main
def collect_english(winscp):
    source = os.path.join(winscp, 'source')
    english = {}   # (section, key) -> English
    for name in sorted(os.listdir(os.path.join(source, 'forms'))):
        if name.endswith('.dfm'):
            for key, text in parse_dfm(os.path.join(source, 'forms', name)):
                english[(name, key)] = normalize_text(text)
    for name in sorted(os.listdir(os.path.join(source, 'resource'))):
        if name.endswith('.rc') and name not in SKIPPED_SECTIONS:
            for key, text in parse_rc(os.path.join(source, 'resource', name)):
                english[(name, key)] = normalize_text(text)
    for key, text in DRC.items():
        english[('WinSCP_DRC.rc', key)] = text
    return english


def build_language(code, sections, english):
    """Returns (strings, contexts, number of translated texts)"""
    # votes[context][English] = Counter(translation); context '' = resource strings
    votes = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    translated = 0
    for (section, key), text in english.items():
        if section in SKIPPED_SECTIONS or SKIPPED_KEYS.match(key) or not has_letters(text):
            continue
        value = sections.get(section, {}).get(key)
        if not value:
            continue
        translated += 1
        value = normalize_text(value)
        context = section[:-4] if section.endswith('.dfm') else ''
        pairs = [(text, value)]
        # "Caption|Hint" / "Short hint|Long hint"
        if '|' in text and text.count('|') == value.count('|'):
            pairs += list(zip(text.split('|'), value.split('|')))
        for en, tr in pairs:
            if not en.strip() or not tr.strip() or not has_letters(en):
                continue
            votes[context][en][tr] += 1
    # global map: most frequent translation over all contexts
    totals = collections.defaultdict(collections.Counter)
    for context, texts in votes.items():
        for en, counter in texts.items():
            totals[en].update(counter)

    def best(counter):
        return max(sorted(counter), key=lambda t: counter[t])

    strings = {}
    for en, counter in totals.items():
        strings[en] = best(counter)
    contexts = {}
    for context, texts in votes.items():
        if not context:
            continue
        for en, counter in texts.items():
            tr = best(counter)
            if tr != strings[en]:
                contexts.setdefault(context, {})[en] = tr

    # mnemonic-free variants (only where they do not collide with a real text)
    def add_variants(target):
        variants = {}
        for en, tr in target.items():
            if has_mnemonic(en):
                bare = strip_mnemonic(en)
                if bare and bare not in target:
                    variants.setdefault(bare, collections.Counter())[strip_mnemonic(tr)] += 1
        for bare, counter in variants.items():
            target[bare] = best(counter)

    add_variants(strings)
    for context in contexts.values():
        add_variants(context)
        for en in [en for en, tr in context.items() if strings.get(en) == tr]:
            del context[en]

    # identical texts carry no information (except to override the global map in a context)
    strings = {en: tr for en, tr in strings.items() if en != tr}
    contexts = {c: dict(sorted(t.items())) for c, t in sorted(contexts.items()) if t}
    return dict(sorted(strings.items())), contexts, translated


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser()
    parser.add_argument('--winscp', required=True, help='path to winscp-master')
    parser.add_argument('--root', default=os.path.dirname(here), help='NixSCP source tree')
    args = parser.parse_args()

    english = collect_english(args.winscp)
    authors = parse_translation(os.path.join(args.winscp, 'translations', 'version.ini')).get('Author', {})

    out_dir = os.path.join(args.root, 'resources', 'translations')
    os.makedirs(out_dir, exist_ok=True)
    for name in os.listdir(out_dir):
        if name.endswith('.json'):
            os.remove(os.path.join(out_dir, name))

    built = {}
    for code in sorted(LANGUAGES):
        path = os.path.join(args.winscp, 'translations', code + '.ini')
        if not os.path.exists(path):
            print('missing %s' % path, file=sys.stderr)
            continue
        built[code] = build_language(code, parse_translation(path), english)
    # TRANSLATION_COMPLETENESS: relative to the most complete translation (some
    # English texts are not translatable, e.g. component names as Text)
    most = max(translated for _, _, translated in built.values()) or 1

    index = []
    files = []
    total_size = 0
    for code, (strings, contexts, translated) in built.items():
        locale, lcid, english_name, native = LANGUAGES[code]
        completeness = min(100, int(100.0 * translated / most))
        header = {
            'lang': code,
            'name': native,
            'english': english_name,
            'locale': locale,
            'lcid': lcid,
            'completeness': completeness,
            'rtl': code in RTL,
            'authors': authors.get(code, '').replace('"', ''),
        }
        document = dict(header)
        document['strings'] = strings
        document['contexts'] = contexts
        file_name = code.lower() + '.json'
        with open(os.path.join(out_dir, file_name), 'w', encoding='utf-8', newline='\n') as f:
            json.dump(document, f, ensure_ascii=False, indent=0, separators=(',', ':'))
            f.write('\n')
        size = os.path.getsize(os.path.join(out_dir, file_name))
        total_size += size
        files.append(file_name)
        index.append(header)
        print('%-4s %-8s %5d strings %4d context entries %3d%% %8d bytes' % (
            code, locale, len(strings), sum(len(t) for t in contexts.values()), completeness, size))

    with open(os.path.join(out_dir, 'index.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(index, f, ensure_ascii=False, indent=1)
        f.write('\n')
    files.append('index.json')
    with open(os.path.join(args.root, 'resources', 'translations.qrc'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('<!-- Generated by tools/build_translations.py from the WinSCP translations (GNU GPL). -->\n')
        f.write('<RCC>\n  <qresource prefix="/translations">\n')
        for name in files:
            f.write('    <file alias="%s">translations/%s</file>\n' % (name, name))
        f.write('  </qresource>\n</RCC>\n')
    with open(os.path.join(out_dir, 'README'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('NixSCP user interface translations, generated by tools/build_translations.py\n'
                'from the WinSCP translations (winscp-master/translations, GNU GPL) and the\n'
                'English texts of WinSCP (source/forms/*.dfm, source/resource/*.rc).\n'
                'Do not edit; regenerate:\n\n'
                '  python3 -I tools/build_translations.py --winscp <winscp-master>\n\n'
                'Translators (years, names) of the WinSCP translations:\n\n')
        for header in index:
            f.write('%-4s %-8s %s\n' % (header['lang'], header['locale'], header['authors']))
    print('%d translations, %d bytes total, written to %s' % (len(files) - 1, total_size, out_dir))
    return 0


if __name__ == '__main__':
    sys.exit(main())
