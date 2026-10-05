-- mod-bots: how common each race and class combination is on a live WotLK realm, used to pick the race
-- and class of new bots. Each faction is weighted on its own; the bot population is then split
-- exactly 50/50 between Alliance and Horde.
--
-- Source: the WarcraftRealms census of 10 June 2010 (patch 3.3), via https://wowwiki-archive.fandom.com/wiki/WoW_Census
-- It gives characters per race and per class for each faction (level 10+, active in the last 30 days,
-- self-reported through the CensusPlus addon), but not per race and class together. The weights below
-- are the race x class counts that match both sets of totals exactly, fitted by iterative proportional
-- fitting over the combinations in playercreateinfo; class totals were scaled to the race totals of
-- their faction, which differ slightly. They are estimated character counts; only ratios matter.

DROP TABLE IF EXISTS `bot_census`;
CREATE TABLE `bot_census` (
  `race` TINYINT UNSIGNED NOT NULL,
  `class` TINYINT UNSIGNED NOT NULL,
  `weight` INT UNSIGNED NOT NULL COMMENT 'Relative within the faction',
  PRIMARY KEY (`race`, `class`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO `bot_census` (`race`, `class`, `weight`) VALUES
(1, 1, 107140),
(1, 2, 260612),
(1, 4, 95350),
(1, 5, 112090),
(1, 6, 132858),
(1, 8, 162673),
(1, 9, 158041),
(2, 1, 72518),
(2, 3, 85020),
(2, 4, 53852),
(2, 6, 71000),
(2, 7, 107868),
(2, 9, 65892),
(3, 1, 30155),
(3, 2, 73351),
(3, 3, 75899),
(3, 4, 26837),
(3, 5, 31548),
(3, 6, 37394),
(4, 1, 81796),
(4, 3, 205877),
(4, 4, 72795),
(4, 5, 85575),
(4, 6, 101431),
(4, 11, 325184),
(5, 1, 94516),
(5, 4, 70187),
(5, 5, 97611),
(5, 6, 92537),
(5, 8, 100344),
(5, 9, 85880),
(6, 1, 59266),
(6, 3, 69484),
(6, 6, 58026),
(6, 7, 88157),
(6, 11, 284053),
(7, 1, 49997),
(7, 4, 44495),
(7, 6, 61999),
(7, 8, 75912),
(7, 9, 73751),
(8, 1, 43540),
(8, 3, 51046),
(8, 4, 32332),
(8, 5, 44966),
(8, 6, 42628),
(8, 7, 64764),
(8, 8, 46225),
(10, 2, 318906),
(10, 3, 143323),
(10, 4, 90781),
(10, 5, 126251),
(10, 6, 119689),
(10, 8, 129786),
(10, 9, 111078),
(11, 1, 39325),
(11, 2, 95655),
(11, 3, 98978),
(11, 5, 41141),
(11, 6, 48764),
(11, 7, 226990),
(11, 8, 59707);
