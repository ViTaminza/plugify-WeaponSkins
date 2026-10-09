#!/usr/bin/env python3
"""Root-only test fixture helper. Credentials stay in a private temporary client file.

Default is read-only inspection. --seed changes only this tester's server_id=1 rows.
Existing rows are saved privately before replacement; production databases are unreachable
through this helper because the database argument is always the literal 'mirrors'.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parents[1]
REPO = HERE.parents[1]
STEAM_ID = '76561198042508552'
EXT_DDL = '''CREATE TABLE IF NOT EXISTS sc_skins_ext (
player_id int(11) NOT NULL, server_id int(11) NOT NULL, team int(11) NOT NULL,
weapon_index int(11) NOT NULL, quality int(11) NOT NULL DEFAULT 0,
sticker_4 varchar(96) NOT NULL DEFAULT '0;0;0;0;0;0;0',
sticker_5 varchar(96) NOT NULL DEFAULT '0;0;0;0;0;0;0',
stickers_full varchar(768) DEFAULT NULL, keychain_seed int(11) NOT NULL DEFAULT 0,
updated_at int(11) NOT NULL DEFAULT 0,
PRIMARY KEY(player_id,server_id,team,weapon_index))
ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;'''


def seed_sql():
    schema = (HERE / 'tests/fixtures/pisex-schema.sql').read_text().replace('{{PREFIX}}', 'sc_')
    schema = schema.replace('CREATE TABLE sc_', 'CREATE TABLE IF NOT EXISTS sc_')
    statements = [schema, EXT_DDL, 'START TRANSACTION;',
                  "INSERT INTO sc_servers (id,name) SELECT 1,'WeaponSkins 27035' WHERE NOT EXISTS (SELECT 1 FROM sc_servers WHERE id=1);",
                  f"INSERT INTO sc_player(name,steamid) SELECT 'WeaponSkins tester','{STEAM_ID}' WHERE NOT EXISTS (SELECT 1 FROM sc_player WHERE steamid='{STEAM_ID}');",
                  f"SET @ws_player=(SELECT id FROM sc_player WHERE steamid='{STEAM_ID}' ORDER BY id LIMIT 1);"]
    for table in ('items', 'skins', 'skins_ext'):
        statements.append(f'DELETE FROM sc_{table} WHERE player_id=@ws_player AND server_id=1;')
    for team, agent in ((0, 5105), (1, 5305)):
        statements.append(f'INSERT INTO sc_items(player_id,server_id,team,agent,music,coin,knife,glove) VALUES(@ws_player,1,{team},{agent},3,6001,507,5030);')
        for definition, paint, label in ((7, 180, 'AK Fire Serpent'), (9, 344, 'AWP Dragon Lore'),
                                         (60, 106, 'M4A1-S Vaporwave'), (507, 38, 'Karambit Fade'),
                                         (5030, 10018, 'Sport Gloves')):
            stat = int(definition != 5030)
            statements.append(f"INSERT INTO sc_skins(player_id,server_id,team,weapon_index,skin,stattrack,stattrack_count,stickers,tag,keychain) VALUES(@ws_player,1,{team},{definition},'{paint};42;0.05',{stat},100,'1;2;0;0','WS C++ {label}','1;0;0;0');")
            full = json.dumps(['1;0;0;0;0;1;0', '2;0;0;0;0;1;0', '0;0;0;0;0;0;0',
                               '0;0;0;0;0;0;0', '1;0;0;0;0;1;0', '2;0;0;0;0;1;0'], separators=(',', ':'))
            statements.append(f"INSERT INTO sc_skins_ext(player_id,server_id,team,weapon_index,quality,sticker_4,sticker_5,stickers_full,keychain_seed,updated_at) VALUES(@ws_player,1,{team},{definition},{9 if stat else 0},'1;0;0;0;0;1;0','2;0;0;0;0;1;0','{full}',42,UNIX_TIMESTAMP());")
    statements.append('COMMIT;')
    return '\n'.join(statements)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seed', action='store_true')
    parser.add_argument('--sql-only', action='store_true', help='Print non-secret fixture SQL for local validation')
    args = parser.parse_args()
    if args.sql_only:
        print(seed_sql())
        return
    source = (REPO / 'ref/prod-27016/addons/configs/databases.cfg').read_text()
    values = {name: re.search(r'"' + name + r'"\s*"([^"]*)"', source).group(1)
              for name in ('host', 'user', 'pass')}
    def quote(value):
        return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\r', '\\r') + '"'
    with tempfile.TemporaryDirectory(prefix='ws-mirrors-') as private_dir:
        client = Path(private_dir) / 'client.cnf'
        client.write_text('[client]\nhost=' + quote(values['host']) + '\nuser=' + quote(values['user']) +
                          '\npassword=' + quote(values['pass']) + '\nport=3306\nprotocol=tcp\n')
        os.chmod(client, 0o600)
        def query(sql):
            result = subprocess.run(['mariadb', '--defaults-file=' + str(client), '--connect-timeout=5',
                                     '--batch', '--raw', '--database=mirrors'], input=sql, text=True,
                                    capture_output=True, timeout=30)
            if result.returncode:
                code = re.search(r'ERROR\s+(\d+)', result.stderr)
                diagnostic = code.group(1) if code else 'unknown'
                raise SystemExit(f'mirrors query failed (client exit {result.returncode}, MySQL error {diagnostic}); credentials suppressed')
            return result.stdout
        if query('SELECT DATABASE();').splitlines()[-1] != 'mirrors':
            raise SystemExit('unexpected database')
        tables = query("SELECT TABLE_NAME FROM information_schema.tables WHERE TABLE_SCHEMA='mirrors' AND TABLE_NAME IN ('sc_player','sc_servers','sc_skins','sc_items','sc_skins_ext');")
        print(tables, end='')
        existing = set(tables.splitlines()[1:])
        backups = {}
        if 'sc_player' in existing:
            print(query(f"SELECT id,steamid FROM sc_player WHERE steamid='{STEAM_ID}';"), end='')
            for table in ('items', 'skins', 'skins_ext'):
                if 'sc_' + table in existing:
                    backups[table] = query(f"SELECT * FROM sc_{table} WHERE player_id IN (SELECT id FROM sc_player WHERE steamid='{STEAM_ID}') AND server_id=1;")
        if not args.seed:
            print('Read-only inspection. Use --seed to create tester rows in mirrors.')
            return
        backup = REPO / 'handoff/weaponskins/analysis/implementation-20261008/mirrors-tester-backup.json'
        if not backup.exists():
            fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as output:
                json.dump(backups, output, ensure_ascii=False, indent=2)
        query(seed_sql())
        print(query(f"SELECT team,weapon_index,skin,stattrack_count FROM sc_skins WHERE player_id=(SELECT id FROM sc_player WHERE steamid='{STEAM_ID}' ORDER BY id LIMIT 1) AND server_id=1 ORDER BY team,weapon_index;"), end='')


if __name__ == '__main__':
    main()
