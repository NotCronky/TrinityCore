-- mod-progression: the patches a character can be in, and one hidden quest per patch.
--
-- A character's patch is stored as one completed hidden quest (quest_id below): the conditions table
-- and other quest checks can then gate content by patch without code. Only the current patch's quest
-- is kept, so "complete N quests" achievements count one extra quest at most.

DROP TABLE IF EXISTS `progression_patch`;
CREATE TABLE `progression_patch` (
  `id` TINYINT UNSIGNED NOT NULL COMMENT 'Order of progression',
  `version` VARCHAR(8) NOT NULL COMMENT 'As typed in .patch commands, e.g. 1.6',
  `name` VARCHAR(48) NOT NULL DEFAULT '',
  `expansion` TINYINT UNSIGNED NOT NULL COMMENT '0 Vanilla, 1 TBC, 2 WotLK',
  `level_cap` TINYINT UNSIGNED NOT NULL,
  `arena_season` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = no arena',
  `quest_id` INT UNSIGNED NOT NULL COMMENT 'Hidden quest marking a character in this patch',
  `unlocks` VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'Shown before advancing',
  `removes` VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'Content that ends on entering this patch; shown before advancing',
  PRIMARY KEY (`id`),
  UNIQUE KEY `version` (`version`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO `progression_patch` (`id`, `version`, `name`, `expansion`, `level_cap`, `arena_season`, `quest_id`, `unlocks`, `removes`) VALUES
(1,  '1.1',   '',                           0, 60, 0, 90001, 'The launch world and dungeons, Molten Core, Onyxia''s Lair', ''),
(2,  '1.2',   'Mysteries of Maraudon',      0, 60, 0, 90002, 'Maraudon', ''),
(3,  '1.3',   'Ruins of the Dire Maul',     0, 60, 0, 90003, 'Dire Maul, Azuregos, Lord Kazzak', ''),
(4,  '1.4',   'The Call to War',            0, 60, 0, 90004, 'The honor system and PvP ranks', ''),
(5,  '1.5',   'Battlegrounds',              0, 60, 0, 90005, 'Alterac Valley, Warsong Gulch', ''),
(6,  '1.6',   'Assault on Blackwing Lair',  0, 60, 0, 90006, 'Blackwing Lair, the Darkmoon Faire', ''),
(7,  '1.7',   'Rise of the Blood God',      0, 60, 0, 90007, 'Zul''Gurub, Arathi Basin', ''),
(8,  '1.8',   'Dragons of Nightmare',       0, 60, 0, 90008, 'The Emerald Dragons, the Silithus revamp', ''),
(9,  '1.9',   'The Gates of Ahn''Qiraj',    0, 60, 0, 90009, 'The Ahn''Qiraj war effort and gates, Ruins and Temple of Ahn''Qiraj', ''),
(10, '1.10',  'Storms of Azeroth',          0, 60, 0, 90010, 'Dungeon set 2 quests', ''),
(11, '1.11',  'Shadow of the Necropolis',   0, 60, 0, 90011, 'Naxxramas (level 60), the Scourge Invasion', ''),
(12, '1.12',  'Drums of War',               0, 60, 0, 90012, 'World PvP objectives in Silithus and the Eastern Plaguelands', ''),
(13, '2.0',   'The Burning Crusade',        1, 70, 1, 90013, 'Outland, level 70, Blood Elves and Draenei, Karazhan, Gruul''s Lair, Magtheridon''s Lair, Serpentshrine Cavern, Tempest Keep, arena season 1', 'Vanilla PvP ranks, the Scourge Invasion'),
(14, '2.1',   'The Black Temple',           1, 70, 2, 90014, 'Black Temple, Ogri''la, Skettis, Netherwing, arena season 2', ''),
(15, '2.2',   'Voice Chat!',                1, 70, 2, 90015, 'Nothing new to play', ''),
(16, '2.3',   'The Gods of Zul''Aman',      1, 70, 3, 90016, 'Zul''Aman, arena season 3', ''),
(17, '2.4',   'Fury of the Sunwell',        1, 70, 4, 90017, 'Sunwell Plateau, the Isle of Quel''Danas, Magisters'' Terrace, arena season 4', ''),
(18, '3.0',   'Wrath of the Lich King',     2, 80, 5, 90018, 'Northrend, level 80, Death Knights, Naxxramas (level 80), Obsidian Sanctum, Eye of Eternity, Wintergrasp, arena season 5', 'Naxxramas (level 60)'),
(19, '3.1',   'Secrets of Ulduar',          2, 80, 6, 90019, 'Ulduar, dual specialization, the Argent Tournament, Strand of the Ancients, arena season 6', ''),
(20, '3.2',   'Call of the Crusade',        2, 80, 7, 90020, 'Trial of the Crusader, Onyxia''s Lair (level 80), Isle of Conquest, arena season 7', 'Onyxia''s Lair (level 60)'),
(21, '3.3',   'Fall of the Lich King',      2, 80, 8, 90021, 'Icecrown Citadel and its dungeons, the Dungeon Finder, arena season 8', ''),
(22, '3.3.5', 'Defending the Ruby Sanctum', 2, 80, 8, 90022, 'The Ruby Sanctum', '');

-- Flags 1024 (QUEST_FLAGS_TRACKING): never shown in the client's quest log. No quest giver offers these.
DELETE FROM `quest_template` WHERE `ID` BETWEEN 90001 AND 90022;
INSERT INTO `quest_template` (`ID`, `QuestType`, `QuestLevel`, `Flags`, `LogTitle`)
SELECT `quest_id`, 2, 1, 1024, CONCAT('Progression: patch ', `version`) FROM `progression_patch`;
