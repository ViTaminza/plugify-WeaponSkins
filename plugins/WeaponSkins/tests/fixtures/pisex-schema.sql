-- Additive schema for the Pisex (Skinchanger by Pisex) sc_* tables, as the plugin uses it.
--
-- This file is the TEST fixture: the plugin itself creates only the additive sc_skins_ext table
-- (see ExtCreateStatement in src/storage.cpp) and never touches the base schema, which belongs
-- to the website. {{PREFIX}} is replaced by the storage test with a unique table prefix.
--
-- The base tables have NO unique keys - the original's class comment says so explicitly, and
-- the write path relies on select-then-update. Do not add them: a duplicate row is exactly the
-- failure the storage code must survive.

CREATE TABLE {{PREFIX}}servers (
	id INT(11) NOT NULL AUTO_INCREMENT,
	name VARCHAR(64) DEFAULT NULL,
	PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE TABLE {{PREFIX}}player (
	id INT(11) NOT NULL AUTO_INCREMENT,
	name VARCHAR(64) DEFAULT NULL,
	steamid VARCHAR(32) DEFAULT NULL,
	PRIMARY KEY (id),
	KEY steamid (steamid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE TABLE {{PREFIX}}items (
	player_id INT(11) NOT NULL,
	server_id INT(11) NOT NULL,
	team INT(11) NOT NULL DEFAULT 0,
	agent INT(11) NOT NULL DEFAULT 0,
	music INT(11) NOT NULL DEFAULT 0,
	coin INT(11) NOT NULL DEFAULT 0,
	knife INT(11) NOT NULL DEFAULT 0,
	glove INT(11) NOT NULL DEFAULT 0,
	KEY player_server (player_id, server_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

CREATE TABLE {{PREFIX}}skins (
	player_id INT(11) NOT NULL,
	server_id INT(11) NOT NULL,
	team INT(11) NOT NULL DEFAULT 0,
	weapon_index INT(11) NOT NULL DEFAULT 0,
	skin VARCHAR(64) DEFAULT NULL,
	stattrack INT(11) NOT NULL DEFAULT 0,
	stattrack_count INT(11) NOT NULL DEFAULT 0,
	stickers VARCHAR(64) DEFAULT NULL,
	tag VARCHAR(64) DEFAULT NULL,
	keychain VARCHAR(64) DEFAULT NULL,
	KEY player_server (player_id, server_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
