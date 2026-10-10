# Adds the records for players on the map and the compass to GameFiles/Skyrim/SkyrimTogether.esp
# (VR_TODO, "Players on the map").
#
#   python Tools/VR/player-markers.py
#
# The plugin's other records were made by hand in the Creation Kit; these are written here so they can be read and
# made again. The script refuses to run twice (the records are found by their editor ids), and keeps the plugin's
# header in step: its record count and next object id.
#
# What it adds, at fixed object ids the client uses (Services/Generic/PlayerMarkerService.cpp):
#   0x005000  QUST  STR_FellowTravellers        misc quest "Fellow travellers", not started with the game. Objective
#                                               N (1 to 7) points at alias N-1, which is forced to party marker N; its
#                                               text is that marker's name, which the client sets ("Seen, near
#                                               Whiterun").
#   0x005001  CELL  STR_FellowTravellersHolding a small interior nobody can reach, where the markers wait.
#   0x005010+ ACTI  STR_PartyMarkerBase1..7     one activator per party marker, no model: its name is the
#                                               marker's name, renamed by the client.
#   0x005020+ REFR  STR_PartyMarker1..7         persistent, in the holding cell until the client moves them.
#   0x005040+ REFR  STR_PlayerMapMarker1..32    persistent map markers for players outside the party: a world-map
#                                               icon each, never on the compass. Named and placed by the client.
import struct
import sys
from pathlib import Path

PLUGIN = Path(__file__).resolve().parents[2] / 'GameFiles' / 'Skyrim' / 'SkyrimTogether.esp'

MOD = 0x02000000  # this plugin's own records: after its masters Skyrim.esm (00) and Update.esm (01)
QUEST_ID = 0x005000
CELL_ID = 0x005001
ACTI_FIRST = 0x005010
PARTY_MARKER_FIRST = 0x005020
MAP_MARKER_FIRST = 0x005040
PARTY_MARKERS = 7
MAP_MARKERS = 32
NEXT_OBJECT_ID = 0x005060

MAP_MARKER_BASE = 0x00000010  # Skyrim.esm MapMarker
FORM_VERSION = 44             # Skyrim SE's, as the plugin's other records
QUEST_TYPE_MISC = 6


def sub(sig, data=b''):
    return sig.encode() + struct.pack('<H', len(data)) + data


def zstr(text):
    return text.encode('cp1252') + b'\0'


def record(sig, form_id, body, flags=0):
    return sig.encode() + struct.pack('<IIIIHH', len(body), flags, form_id, 0, FORM_VERSION, 0) + body


def group(label, group_type, contents):
    if isinstance(label, str):
        label = label.encode()
    else:
        label = struct.pack('<I', label)
    return b'GRUP' + struct.pack('<I', 24 + len(contents)) + label + struct.pack('<iHHHH', group_type, 0, 0, 0, 0) + contents


def position(x=0.0, y=0.0, z=0.0):
    return struct.pack('<6f', x, y, z, 0.0, 0.0, 0.0)


PERSISTENT = 0x400


def build_activators():
    out = b''
    for i in range(PARTY_MARKERS):
        body = sub('EDID', zstr(f'STR_PartyMarkerBase{i + 1}')) + sub('OBND', bytes(12)) + sub('FULL', zstr('Fellow traveller'))
        out += record('ACTI', MOD | (ACTI_FIRST + i), body)
    return out


def build_cell():
    cell = record('CELL', MOD | CELL_ID, sub('EDID', zstr('STR_FellowTravellersHolding')) + sub('DATA', struct.pack('<H', 0x0001)))

    refs = b''
    for i in range(PARTY_MARKERS):
        body = sub('EDID', zstr(f'STR_PartyMarker{i + 1}')) + sub('NAME', struct.pack('<I', MOD | (ACTI_FIRST + i))) + sub('DATA', position(i * 64.0))
        refs += record('REFR', MOD | (PARTY_MARKER_FIRST + i), body, PERSISTENT)
    for i in range(MAP_MARKERS):
        body = (sub('EDID', zstr(f'STR_PlayerMapMarker{i + 1}')) + sub('NAME', struct.pack('<I', MAP_MARKER_BASE)) + sub('XMRK')
                + sub('FNAM', bytes([0x01]))  # visible on the map; never "can travel to"
                + sub('FULL', zstr('Fellow traveller')) + sub('TNAM', bytes([0, 0])) + sub('DATA', position(i * 64.0, 512.0)))
        refs += record('REFR', MOD | (MAP_MARKER_FIRST + i), body, PERSISTENT)

    children = group(MOD | CELL_ID, 6, group(MOD | CELL_ID, 8, refs))
    # Interior cells sit in a block and sub-block named after the last two decimal digits of their object id.
    block = CELL_ID % 10
    subblock = (CELL_ID // 10) % 10
    return group('CELL', 0, group(block, 2, group(subblock, 3, cell + children)))


def build_quest():
    body = sub('EDID', zstr('STR_FellowTravellers')) + sub('FULL', zstr('Fellow travellers'))
    # Flags (none: started and stopped by the client), priority, form version, unused, type.
    body += sub('DNAM', struct.pack('<HBBII', 0, 0, 0xFF, 0, QUEST_TYPE_MISC))
    body += sub('NEXT')
    for i in range(PARTY_MARKERS):
        body += sub('QOBJ', struct.pack('<H', i + 1))
        body += sub('FNAM', struct.pack('<I', 0))
        body += sub('NNAM', zstr(f'<Alias=Member{i + 1}>'))
        body += sub('QSTA', struct.pack('<iI', i, 0))
    body += sub('ANAM', struct.pack('<I', PARTY_MARKERS))
    for i in range(PARTY_MARKERS):
        body += sub('ALST', struct.pack('<I', i))
        body += sub('ALID', zstr(f'Member{i + 1}'))
        body += sub('FNAM', struct.pack('<I', 0))
        body += sub('ALFR', struct.pack('<I', MOD | (PARTY_MARKER_FIRST + i)))
        body += sub('VTCK', struct.pack('<I', 0))
        body += sub('ALED')
    return record('QUST', MOD | QUEST_ID, body)


def split_groups(data):
    """The TES4 header record, then each top-level group as (label, bytes)."""
    header_size = 24 + struct.unpack_from('<I', data, 4)[0]
    header = data[:header_size]
    groups = []
    offset = header_size
    while offset < len(data):
        assert data[offset:offset + 4] == b'GRUP', f'expected a group at {offset}'
        size = struct.unpack_from('<I', data, offset + 4)[0]
        groups.append((data[offset + 8:offset + 12].decode(), data[offset:offset + size]))
        offset += size
    return header, groups


def count_records(data):
    """Records and groups, as the header counts them."""
    count = 0
    offset = 0
    while offset < len(data):
        sig = data[offset:offset + 4]
        size = struct.unpack_from('<I', data, offset + 4)[0]
        count += 1
        if sig == b'GRUP':
            count += count_records(data[offset + 24:offset + size])
            offset += size
        else:
            offset += 24 + size
    return count


def main():
    data = PLUGIN.read_bytes()
    if b'STR_FellowTravellers\0' in data:
        print(f'{PLUGIN.name} already has the player marker records; nothing done.')
        return 0

    header, groups = split_groups(data)
    by_label = dict(groups)
    order = [label for label, _ in groups]

    # Skyrim.esm's group order: SPEL, ACTI, ..., CELL, ..., QUST.
    by_label['ACTI'] = group('ACTI', 0, build_activators())
    order.insert(order.index('SPEL') + 1, 'ACTI')
    by_label['CELL'] = build_cell()
    order.insert(order.index('QUST'), 'CELL')
    quest_group = by_label['QUST']
    contents = quest_group[24:] + build_quest()
    by_label['QUST'] = quest_group[:4] + struct.pack('<I', 24 + len(contents)) + quest_group[8:24] + contents

    body = b''.join(by_label[label] for label in order)

    # Header: HEDR is version (float), number of records and groups, next object id.
    hedr_at = header.index(b'HEDR')
    version, = struct.unpack_from('<f', header, hedr_at + 6)
    new_header = bytearray(header)
    struct.pack_into('<fII', new_header, hedr_at + 6, version, count_records(body), NEXT_OBJECT_ID)

    PLUGIN.write_bytes(bytes(new_header) + body)
    print(f'{PLUGIN.name}: added the quest, the holding cell, {PARTY_MARKERS} party markers and {MAP_MARKERS} map markers; '
          f'{count_records(body)} records and groups')
    return 0


if __name__ == '__main__':
    sys.exit(main())
