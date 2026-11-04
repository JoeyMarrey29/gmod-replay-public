"""Extracts job data (teams, team colours, rank names) from the Perpheads Lua unpacked from a demo.

Usage: python export_jobs.py <lua_root> <out.json>
"""
import json
import os
import re
import sys

root = sys.argv[1]
out_path = sys.argv[2]


def read_all():
    for folder, _, files in os.walk(root):
        for name in files:
            if name.endswith('.lua'):
                path = os.path.join(folder, name)
                with open(path, encoding='utf-8', errors='replace') as handle:
                    yield path, handle.read()


files = list(read_all())

# Team ids come from sh_teams.lua; names and colours from team.SetUp in sh_jobs.lua.
team_ids = {}
for path, text in files:
    if path.endswith(os.path.join('core', 'sh_teams.lua')):
        team_ids = {m.group(1): int(m.group(2)) for m in re.finditer(r'(TEAM_\w+)\s*=\s*(\d+)', text)}

teams = {}
for path, text in files:
    if path.endswith(os.path.join('jobs', 'sh_jobs.lua')):
        for m in re.finditer(
                r'team\.SetUp\((TEAM_\w+),\s*"([^"]+)",\s*Color\((\d+),\s*(\d+),\s*(\d+)\)\)', text):
            teams[team_ids[m.group(1)]] = {
                'id': team_ids[m.group(1)], 'key': m.group(1), 'name': m.group(2),
                'color': [int(m.group(3)), int(m.group(4)), int(m.group(5))],
            }

# Rank tables: <Table>.RANK_X = n, then <Table>.RankNames / RankShortNames blocks.
ranks = {}
for path, text in files:
    for table in ('PoliceWhitelist', 'MedicJob', 'FirefighterJob'):
        constants = {m.group(1): int(m.group(2))
                     for m in re.finditer(r'%s\.(RANK_\w+)\s*=\s*(\d+)' % table, text)}
        if not constants:
            continue
        entry = ranks.setdefault(table, {'names': {}, 'short': {}})
        for kind, field in (('names', 'RankNames'), ('short', 'RankShortNames')):
            block = re.search(r'%s\.%s\s*=\s*\{(.*?)\n\}' % (table, field), text, re.S)
            if not block:
                continue
            for m in re.finditer(r'\[%s\.(RANK_\w+)\]\s*=\s*"([^"]*)"' % table, block.group(1)):
                if m.group(1) in constants:
                    entry[kind][str(constants[m.group(1)])] = m.group(2)

radio_channels = {}
for path, text in files:
    block = re.search(r'GovernmentRadioSystem\.ChannelSettings\s*=\s*\{(.*?)\n\}', text, re.S)
    if block:
        for m in re.finditer(r'\[(\d+)\]\s*=\s*\{[^{}]*?name\s*=\s*"([^"]*)"', block.group(1), re.S):
            radio_channels[m.group(1)] = m.group(2)

npcs = {}
for path, text in files:
    # NPC definitions: NPC.Name = "..." and NPC.ID = n in the same file.
    name = re.search(r'^\s*NPC\.Name\s*=\s*"([^"]*)"', text, re.M)
    npc_id = re.search(r'^\s*NPC\.ID\s*=\s*(\d+)', text, re.M)
    if name and npc_id:
        npcs[npc_id.group(1)] = name.group(1)

result = {
    'teams': {str(k): v for k, v in sorted(teams.items())},
    # NPC_DATABASE ids (perpheads_npc_used) -> NPC.Name.
    'npcs': npcs,
    # GovernmentRadioSystem.ChannelSettings: the fixed radio channels (others are created by
    # the server at run time and named by the demo).
    'radioChannels': radio_channels,
    'ranks': {
        'police': ranks.get('PoliceWhitelist', {}),
        'medic': ranks.get('MedicJob', {}),
        'firefighter': ranks.get('FirefighterJob', {}),
    },
    # Which networked variable (lower-case key) holds the rank for each team.
    'rankVariables': {
        str(team_ids['TEAM_POLICE']): 'policerank',
        str(team_ids['TEAM_MEDIC']): 'medicrank',
        str(team_ids['TEAM_FIREFIGHTER']): 'firefighterrank',
    },
    'dispatcherTeam': team_ids['TEAM_DISPATCHER'],
    'citizenTeam': team_ids['TEAM_CITIZEN'],
}
with open(out_path, 'w', encoding='utf-8') as handle:
    json.dump(result, handle, indent=1)
print('teams', len(teams), {k: len(v.get('names', {})) for k, v in result['ranks'].items()})
