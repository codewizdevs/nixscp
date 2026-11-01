#!/usr/bin/env python3
# Extracts the WinSCP icon set from the WinSCP source tree:
#   source/forms/Glyphs{,120,144,192}.dfm      (TGlyphsModule: ExplorerImages,
#                                              SessionImages, QueueImages)
#   source/forms/Animations{96,120,144,192}.dfm (AnimationImages, DialogImages)
#   source/resource/{Application,Site,SiteFolder,Workspace}.ico
# and generates resources/icons.qrc, resources/animations.qrc,
# resources/icons/README, the action -> icon table in src/gui/Icons.cpp and the
# icon name list in src/gui/Icons.h (both between the GENERATED markers).
#
# The images are NOT GPL: see LICENSE_NOTE below (WinSCP license.txt, part B).
#
# Usage: python3 -I tools/extract_glyphs.py [--winscp <winscp-master>] [--root <nixscp>]
# Uses the Python standard library only.

import argparse
import os
import re
import struct
import sys
import zlib

# Glyph module -> pixel size (96 DPI = 16 px, 120 = 20, 144 = 24, 192 = 32;
# TGlyphsModule::UpdatePixelsPerInch picks the module by DPI)
GLYPH_FILES = [('Glyphs.dfm', 16), ('Glyphs120.dfm', 20), ('Glyphs144.dfm', 24), ('Glyphs192.dfm', 32)]
ANIMATION_FILES = [('Animations96.dfm', 32), ('Animations120.dfm', 40), ('Animations144.dfm', 48),
                   ('Animations192.dfm', 64)]
ICO_FILES = [('Application.ico', 'nixscp'), ('Site.ico', 'site'), ('SiteFolder.ico', 'site-folder'),
             ('Workspace.ico', 'workspace')]

# Application and site icons (source/resource/*.ico); all sizes of the .ico
ICO_DESCRIPTIONS = {
  'nixscp': 'Application icon (Application.ico)',
  'site': 'Site (Site.ico)',
  'site-folder': 'Site folder (SiteFolder.ico)',
  'workspace': 'Workspace (Workspace.ico)',
}

LICENSE_NOTE = """\
The WinSCP icon set is (c) its respective owners (see WinSCP's license.txt,
part B "License of WinSCP Icon Set") and is not covered by the GNU General
Public License that covers the NixSCP code.

It is used only when NixSCP is configured with -DNIXSCP_WINSCP_ICONS=ON.
The default build uses the desktop icon theme (freedesktop names) and QStyle
standard icons instead. NixSCP's own application icon (resources/appicon,
data/nixscp.svg) is original artwork under NixSCP's license.
"""

# Meaningful names of the glyph indexes. Indexes 0-116 are TGlyphsModule::ExplorerImages
# (the indexes the actions in NonVisual.dfm refer to), 117-118 SessionImages (index 0-1),
# 119-127 QueueImages (index 0-8). Unused/blank glyphs are not named (not in the qrc).
GLYPH_NAMES = {
  2: 'delete',
  3: 'rename',
  4: 'properties',
  5: 'new-folder',
  6: 'back',
  7: 'forward',
  8: 'view-large-icons',
  # Named by the actions using them (RemoteSmallIconAction = 9, RemoteListAction = 10),
  # although the Glyphs.dfm image names are 9 "List view", 10 "Small icons view"
  9: 'view-small-icons',
  10: 'view-list',
  11: 'view-details',
  12: 'parent-dir',
  13: 'root-dir',
  14: 'root-dir-local',
  15: 'home-dir',
  16: 'refresh',
  17: 'server-info',
  18: 'open-dir',
  19: 'select',
  20: 'unselect',
  21: 'select-all',
  22: 'invert-selection',
  23: 'clear-selection',
  25: 'new-session',
  26: 'close-tab',
  27: 'sites',
  28: 'preferences',
  29: 'change-path-remote',
  30: 'change-path-local',
  31: 'sort-name',
  32: 'sort-ext',
  33: 'sort-changed',
  34: 'sort-type',
  35: 'sort-size',
  36: 'sort-rights',
  37: 'sort-toggle',
  38: 'sort-owner',
  39: 'sort-group',
  40: 'sort-descending',
  41: 'sort-ascending',
  42: 'homepage',
  43: 'save-session',
  44: 'column-name',
  45: 'column-ext',
  46: 'column-type',
  47: 'column-size',
  48: 'column-changed',
  49: 'column-rights',
  50: 'column-owner',
  51: 'column-group',
  52: 'compare',
  53: 'keep-up-to-date',
  54: 'bookmark',
  55: 'console',
  56: 'explore',
  57: 'edit',
  58: 'open',
  59: 'synchronize-browsing',
  60: 'link',
  61: 'quit',
  62: 'opened-tabs',
  63: 'check-updates',
  64: 'putty',
  65: 'about',
  66: 'synchronize',
  67: 'queue-query',
  68: 'queue-error',
  69: 'queue-prompt',
  70: 'queue-execute',
  71: 'queue-delete',
  72: 'queue-up',
  73: 'queue-down',
  74: 'queue',
  75: 'paste',
  76: 'tree',
  77: 'new-file',
  78: 'duplicate',
  79: 'help',
  80: 'check-updates-results',
  81: 'check-updates-disabled',
  82: 'column-link-target',
  83: 'queue-pause',
  84: 'queue-pause-all',
  85: 'queue-resume-all',
  86: 'restore-selection',
  87: 'queue-disconnect-once-empty',
  88: 'upload',
  89: 'download',
  90: 'command',
  91: 'duplicate-session',
  92: 'filter',
  93: 'queue-shutdown-once-empty',
  94: 'queue-idle-once-empty',
  95: 'find-files',
  96: 'queue-enable',
  97: 'download-move',
  98: 'upload-move',
  99: 'queue-delete-all-done',
  100: 'move',
  101: 'workspaces',
  102: 'save-workspace',
  # Session tab image and its colour mask (TCustomScpExplorerForm::AddFixedSessionImages,
  # SiteImageIndex/SiteColorMaskImageIndex); "site" is Site.ico
  103: 'session-tab',
  104: 'session-tab-color-mask',
  105: 'queue-suspend-once-empty',
  106: 'queue-delete-all',
  107: 'download-queue',
  108: 'upload-queue',
  109: 'speed-limit',
  110: 'tips',
  111: 'copy',
  112: 'duplicate-r-to-l',
  113: 'move-r-to-l',
  114: 'local-browser',
  115: 'new-local-tab',
  116: 'disconnect',
  # SessionImages
  118: 'session-encrypted',
  # QueueImages (TQueueController list view)
  119: 'queue-item-download',
  120: 'queue-item-download-move',
  121: 'queue-item-upload',
  122: 'queue-item-upload-move',
  123: 'queue-item-query',
  124: 'queue-item-error',
  125: 'queue-item-prompt',
  126: 'queue-item-paused',
  127: 'queue-item-delete',
}

# Actions whose image WinSCP sets at run time (TNonVisualDataModule::ExplorerActionsUpdate);
# the table uses the image shown with the default configuration instead of the .dfm one
RUNTIME_ACTION_ICONS = {
  # TScpCommanderForm::GetNewTabActionImageIndex: NewRemoteTabAction's image,
  # as DefaultToNewRemoteTab defaults to true
  'NewTabAction': 'new-session',
  # Image of the remote panel's view style; Commander panels default to dvsReport
  'RemoteCycleStyleAction': 'view-details',
}

# Only these PNG chunks are kept (strips the Adobe XMP metadata most glyphs carry)
KEEP_CHUNKS = {b'IHDR', b'PLTE', b'tRNS', b'IDAT', b'IEND'}
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'


def delphi_string(Text):
  """Decodes a Delphi DFM string literal ('abc'#39'def' + ...)."""
  Result = ''
  Pos = 0
  while Pos < len(Text):
    Ch = Text[Pos]
    if Ch == "'":
      End = Pos + 1
      while True:
        End = Text.index("'", End)
        if End + 1 < len(Text) and Text[End + 1] == "'":
          End += 2
          continue
        break
      Result += Text[Pos + 1:End].replace("''", "'")
      Pos = End + 1
    elif Ch == '#':
      M = re.match(r'#(\d+)', Text[Pos:])
      Result += chr(int(M.group(1)))
      Pos += len(M.group(0))
    else:
      Pos += 1
  return Result


def parse_png_image_lists(FileName):
  """Returns {list name: (width, height, [(item name, png bytes), ...])}"""
  Lists = {}
  Current = None
  Item = None
  HexData = None
  with open(FileName, encoding='latin-1') as F:
    for Line in F:
      Stripped = Line.strip()
      if HexData is not None:
        Done = Stripped.endswith('}')
        HexData.append(Stripped.rstrip('}'))
        if Done:
          Item[1] = bytes.fromhex(''.join(HexData))
          HexData = None
        continue
      M = re.match(r'object (\w+): (\w+)$', Stripped)
      if M:
        if M.group(2) == 'TPngImageList':
          Current = [16, 16, []]
          Lists[M.group(1)] = Current
        else:
          Current = None
        continue
      if Current is None:
        continue
      M = re.match(r'(Width|Height) = (\d+)$', Stripped)
      if M and Item is None:
        Current[0 if M.group(1) == 'Width' else 1] = int(M.group(2))
      elif Stripped == 'item':
        Item = ['', None]
      elif Stripped == 'end' and Item is not None:
        Current[2].append((Item[0], Item[1]))
        Item = None
      elif Item is not None and Stripped.startswith('Name = '):
        Item[0] = delphi_string(Stripped[len('Name = '):])
      elif Item is not None and Stripped.startswith('PngImage.Data = {'):
        Rest = Stripped[len('PngImage.Data = {'):]
        HexData = []
        if Rest:
          HexData.append(Rest.rstrip('}'))
          if Rest.endswith('}'):
            Item[1] = bytes.fromhex(''.join(HexData))
            HexData = None
  return {Name: (W, H, Items) for Name, (W, H, Items) in Lists.items()}


def png_chunks(Data):
  if not Data.startswith(PNG_SIGNATURE):
    raise ValueError('not a PNG')
  Pos = len(PNG_SIGNATURE)
  while Pos < len(Data):
    Length, Type = struct.unpack('>I4s', Data[Pos:Pos + 8])
    yield Type, Data[Pos + 8:Pos + 8 + Length]
    Pos += 12 + Length


def png_chunk(Type, Body):
  return struct.pack('>I', len(Body)) + Type + Body + struct.pack('>I', zlib.crc32(Type + Body) & 0xffffffff)


def strip_png(Data):
  return PNG_SIGNATURE + b''.join(png_chunk(T, B) for T, B in png_chunks(Data) if T in KEEP_CHUNKS)


def png_size(Data):
  for Type, Body in png_chunks(Data):
    if Type == b'IHDR':
      return struct.unpack('>II', Body[:8])
  raise ValueError('no IHDR')


def encode_png_rgba(Width, Height, Rows):
  """Rows: list of bytes (RGBA, Width * 4 each), top-down"""
  Raw = b''.join(b'\x00' + Row for Row in Rows)
  return (PNG_SIGNATURE + png_chunk(b'IHDR', struct.pack('>IIBBBBB', Width, Height, 8, 6, 0, 0, 0)) +
          png_chunk(b'IDAT', zlib.compress(Raw, 9)) + png_chunk(b'IEND', b''))


def dib_to_png(Data, Width, Height):
  """Converts an ICO BMP entry (BITMAPINFOHEADER + XOR bitmap + AND mask) to PNG.
  Supports 32 bpp (all WinSCP icons) and 24 bpp."""
  HeaderSize, DibWidth, DibHeight, _Planes, Bpp = struct.unpack('<IiiHH', Data[:16])
  if Bpp not in (24, 32):
    raise ValueError('unsupported icon bit depth %d' % Bpp)
  Width = DibWidth
  Height = abs(DibHeight) // 2
  Stride = ((Width * Bpp + 31) // 32) * 4
  MaskStride = ((Width + 31) // 32) * 4
  XorOffset = HeaderSize
  MaskOffset = XorOffset + Stride * Height
  Pixels = []
  for Y in range(Height):
    Row = Data[XorOffset + Y * Stride:XorOffset + (Y + 1) * Stride]
    Px = []
    for X in range(Width):
      B, G, R = Row[X * Bpp // 8:X * Bpp // 8 + 3]
      A = Row[X * 4 + 3] if Bpp == 32 else 255
      Px.append([R, G, B, A])
    Pixels.append(Px)
  if Bpp == 24 or all(P[3] == 0 for Row in Pixels for P in Row):
    for Y in range(Height):
      Mask = Data[MaskOffset + Y * MaskStride:MaskOffset + (Y + 1) * MaskStride]
      for X in range(Width):
        Transparent = (Mask[X // 8] >> (7 - X % 8)) & 1
        Pixels[Y][X][3] = 0 if Transparent else 255
  # DIB rows are bottom-up
  Rows = [bytes(V for P in Row for V in P) for Row in reversed(Pixels)]
  return encode_png_rgba(Width, Height, Rows)


def read_ico(FileName):
  """Returns {size: png bytes}"""
  with open(FileName, 'rb') as F:
    Data = F.read()
  _Reserved, Type, Count = struct.unpack('<HHH', Data[:6])
  if Type != 1:
    raise ValueError('%s is not an icon' % FileName)
  Result = {}
  for Index in range(Count):
    W, H, _Colors, _R, _Planes, _Bpp, Size, Offset = struct.unpack('<BBBBHHII', Data[6 + 16 * Index:22 + 16 * Index])
    W = W or 256
    H = H or 256
    Entry = Data[Offset:Offset + Size]
    if Entry.startswith(PNG_SIGNATURE):
      Png = strip_png(Entry)
    else:
      Png = dib_to_png(Entry, W, H)
    Result[png_size(Png)[0]] = Png
  return Result


def slug(Name):
  """'Keep remote directory up to date' -> 'keep-remote-directory-up-to-date'
  (keep in sync with Slug() in src/gui/Icons.cpp)"""
  return re.sub(r'[^a-z0-9]+', '-', Name.lower()).strip('-')


def write_file(Path, Data):
  os.makedirs(os.path.dirname(Path), exist_ok=True)
  Old = None
  if os.path.exists(Path):
    with open(Path, 'rb') as F:
      Old = F.read()
  if Old != Data:
    with open(Path, 'wb') as F:
      F.write(Data)


def remove_stale(Dir, Keep):
  """Deletes the PNGs in Dir that the current run did not write"""
  if os.path.isdir(Dir):
    for Name in os.listdir(Dir):
      if Name.endswith('.png') and Name not in Keep:
        os.remove(os.path.join(Dir, Name))


def replace_generated(Path, Begin, End, Lines):
  """Replaces the lines between the Begin and End marker lines of Path (if it has them)"""
  if not os.path.exists(Path):
    return
  with open(Path, encoding='utf-8') as F:
    Text = F.read()
  if Begin in Text and End in Text:
    Head, Rest = Text.split(Begin, 1)
    _, Tail = Rest.split(End, 1)
    write_file(Path, (Head + Begin + ''.join(L + '\n' for L in Lines) + End + Tail).encode())
  else:
    print('warning: %s has no generated-section markers' % Path, file=sys.stderr)


def parse_actions(FileName):
  """Returns [(action name, caption, image index)] of NonVisual.dfm TActions with ImageIndex"""
  Result = []
  Current = None
  with open(FileName, encoding='latin-1') as F:
    for Line in F:
      Stripped = Line.strip()
      M = re.match(r'object (\w+): (\w+)$', Stripped)
      if M:
        Current = [M.group(1), '', None] if M.group(2) == 'TAction' else None
        if Current:
          Result.append(Current)
        continue
      if Current is None:
        continue
      if Stripped.startswith('Caption = '):
        Current[1] = delphi_string(Stripped[len('Caption = '):])
      elif Stripped.startswith('ImageIndex = '):
        Current[2] = int(Stripped[len('ImageIndex = '):])
  return [tuple(A) for A in Result if A[2] is not None]


def main():
  Here = os.path.dirname(os.path.abspath(__file__))
  Parser = argparse.ArgumentParser()
  Parser.add_argument('--root', default=os.path.dirname(Here))
  Parser.add_argument('--winscp', default=os.path.join(os.path.dirname(os.path.dirname(Here)), 'winscp-master'))
  Parser.add_argument('--cpp', help='Icons.cpp to update (default <root>/src/gui/Icons.cpp)')
  Args = Parser.parse_args()
  Source = os.path.join(Args.winscp, 'source')
  Forms = os.path.join(Source, 'forms')
  Resources = os.path.join(Args.root, 'resources')

  IconsQrc = []   # (alias, file relative to resources/)
  AnimQrc = []

  # --- Glyphs ---
  Reference = None
  Descriptions = {}
  for FileName, Size in GLYPH_FILES:
    Lists = parse_png_image_lists(os.path.join(Forms, FileName))
    Items = []
    for ListName in ('ExplorerImages', 'SessionImages', 'QueueImages'):
      W, H, ListItems = Lists[ListName]
      if (W, H) != (Size, Size):
        raise SystemExit('%s: %s is %dx%d, expected %d' % (FileName, ListName, W, H, Size))
      Items.extend(ListItems)
    Names = [N for N, _ in Items]
    if Reference is None:
      Reference = Names
    elif Names != Reference:
      raise SystemExit('%s: glyph order differs from Glyphs.dfm' % FileName)
    Written = set()
    for Index, (Name, Data) in enumerate(Items):
      # Unnamed glyphs are the blank "(unused)" placeholders
      if Index not in GLYPH_NAMES:
        continue
      Png = strip_png(Data)
      if png_size(Png) != (Size, Size):
        print('warning: %s #%d %r is %dx%d' % ((FileName, Index, Name) + png_size(Png)), file=sys.stderr)
      Rel = 'icons/%d/%d.png' % (Size, Index)
      write_file(os.path.join(Resources, Rel), Png)
      Written.add('%d.png' % Index)
      IconsQrc.append(('%d/%s.png' % (Size, GLYPH_NAMES[Index]), Rel))
      Descriptions[GLYPH_NAMES[Index]] = (Index, Name)
    remove_stale(os.path.join(Resources, 'icons', str(Size)), Written)
  print('glyphs: %d per size, %d named' % (len(Reference), len(GLYPH_NAMES)))

  # --- Application and site icons ---
  IcoSizes = {}
  for FileName, Base in ICO_FILES:
    Pngs = read_ico(os.path.join(Source, 'resource', FileName))
    IcoSizes[Base] = sorted(Pngs)
    for Size, Png in sorted(Pngs.items()):
      Rel = 'icons/app/%s-%d.png' % (Base, Size)
      write_file(os.path.join(Resources, Rel), Png)
      IconsQrc.append(('%d/%s.png' % (Size, Base), Rel))
    if Base == 'nixscp':
      # data/nixscp.png is NixSCP's own icon (original artwork); WinSCP's
      # Application.ico stays in resources/icons/app only
      print('application icon: %s' % ', '.join(str(S) for S in sorted(Pngs)))

  # --- Animations and dialog images ---
  for FileName, Size in ANIMATION_FILES:
    Lists = parse_png_image_lists(os.path.join(Forms, FileName))
    W, H, Frames = Lists['AnimationImages']
    for Name, Data in Frames:
      # Name_FrameIndex_Duration (TFrameAnimation; duration in 10 ms)
      if not re.match(r'^\w+_-?\d+_\d+$', Name):
        raise SystemExit('%s: unexpected animation frame name %r' % (FileName, Name))
      Rel = 'animations/%d/%s.png' % (Size, Name)
      write_file(os.path.join(Resources, Rel), strip_png(Data))
      AnimQrc.append(('animations/%d/%s.png' % (Size, Name), Rel))
    W, H, Images = Lists['DialogImages']
    for Name, Data in Images:
      Rel = 'dialogs/%d/%s.png' % (Size, slug(Name))
      write_file(os.path.join(Resources, Rel), strip_png(Data))
      AnimQrc.append(('dialogs/%d/%s.png' % (Size, slug(Name)), Rel))
    print('%s: %d animation frames, %d dialog images' % (FileName, len(Frames), len(Images)))

  def write_qrc(Path, Prefix, Entries):
    Aliases = [Alias for Alias, _ in Entries]
    Duplicates = sorted(set(A for A in Aliases if Aliases.count(A) > 1))
    if Duplicates:
      raise SystemExit('%s: duplicate aliases %s' % (Path, ', '.join(Duplicates)))
    Lines = ['<!DOCTYPE RCC>', '<!-- Generated by tools/extract_glyphs.py, do not edit -->',
             '<RCC version="1.0">', '  <qresource prefix="%s">' % Prefix]
    for Alias, Rel in Entries:
      Lines.append('    <file alias="%s">%s</file>' % (Alias, Rel))
    Lines += ['  </qresource>', '</RCC>', '']
    write_file(Path, '\n'.join(Lines).encode())

  write_qrc(os.path.join(Resources, 'icons.qrc'), '/icons', IconsQrc)
  write_qrc(os.path.join(Resources, 'animations.qrc'), '/', AnimQrc)

  # --- resources/icons/README and the name list in src/gui/Icons.h ---
  GlyphSizes = '/'.join(str(Size) for _, Size in GLYPH_FILES)
  Readme = [
    'WinSCP icon set used by NixSCP',
    '==============================',
    '',
    'Origin',
    '------',
    'Extracted from the WinSCP source tree (https://winscp.net/, a GPL-3',
    'project) by tools/extract_glyphs.py; do not edit, rerun the script:',
    '',
    '  python3 -I tools/extract_glyphs.py [--winscp <winscp-master>]',
    '',
    '  16/ 20/ 24/ 32/  source/forms/Glyphs.dfm, Glyphs120/144/192.dfm',
    '                   (TGlyphsModule ExplorerImages + SessionImages +',
    '                   QueueImages for 96/120/144/192 DPI); <index>.png is the',
    '                   image index used by the actions in forms/NonVisual.dfm',
    '  app/             source/resource/{Application,Site,SiteFolder,Workspace}.ico',
    '  ../animations/   source/forms/Animations{96,120,144,192}.dfm (AnimationImages)',
    '  ../dialogs/      the same files (DialogImages, LoadDialogImage)',
    '',
    'License',
    '-------',
    LICENSE_NOTE.rstrip('\n'),
    '',
    'Names',
    '-----',
    'Icons::Get("<name>") / resource :/icons/<size>/<name>.png. Glyph sizes',
    '%s px; app icons as listed. Glyphs are described by the captions of' % GlyphSizes,
    'the WinSCP actions using them, unused ones by their WinSCP image name.',
    '',
  ]
  # Describe each glyph by the captions of the WinSCP actions using it (the
  # image names in Glyphs.dfm are partly stale, e.g. #52 "Swap left-right
  # file panels" is the Compare Directories glyph), else by the image name
  Actions = parse_actions(os.path.join(Forms, 'NonVisual.dfm'))
  Captions = {}
  for _, Caption, Index in Actions:
    Caption = Caption.replace('&', '').replace('...', '').strip()
    if Caption not in Captions.setdefault(Index, []):
      Captions[Index].append(Caption)
  NameLines = []
  for Name in sorted(Descriptions):
    Index, Description = Descriptions[Name]
    if Index in Captions:
      Description = '"%s"' % '", "'.join(Captions[Index][:3]) + (', ...' if len(Captions[Index]) > 3 else '')
    NameLines.append(('%-28s #%-3d %s' % (Name, Index, Description)).rstrip())
  for Base in sorted(ICO_DESCRIPTIONS):
    NameLines.append('%-28s      %s, %s px' % (Base, ICO_DESCRIPTIONS[Base], '/'.join(str(S) for S in IcoSizes[Base])))
  Readme += NameLines + ['']
  write_file(os.path.join(Resources, 'icons', 'README'), '\n'.join(Readme).encode())
  HPath = os.path.join(os.path.dirname(Args.cpp) if Args.cpp else os.path.join(Args.root, 'src', 'gui'), 'Icons.h')
  replace_generated(HPath, '  // BEGIN GENERATED ICON NAMES\n', '  // END GENERATED ICON NAMES\n',
                    ['  //   ' + Line for Line in NameLines])

  # --- WinSCP action -> icon table in src/gui/Icons.cpp ---
  Table = []
  for Name, Caption, Index in sorted(Actions, key=lambda A: A[0]):
    if Name in RUNTIME_ACTION_ICONS:
      Table.append('  { "%s", "%s" }, // %s (set at run time)' % (Name, RUNTIME_ACTION_ICONS[Name], Caption.replace('&', '')))
    elif Index in GLYPH_NAMES:
      Table.append('  { "%s", "%s" }, // %s' % (Name, GLYPH_NAMES[Index], Caption.replace('&', '')))
    else:
      print('warning: action %s uses unnamed glyph %d' % (Name, Index), file=sys.stderr)
  CppPath = Args.cpp or os.path.join(Args.root, 'src', 'gui', 'Icons.cpp')
  replace_generated(CppPath, '// BEGIN GENERATED ACTION TABLE\n', '  // END GENERATED ACTION TABLE\n', Table)
  print('actions: %d mapped' % len(Table))


if __name__ == '__main__':
  main()
