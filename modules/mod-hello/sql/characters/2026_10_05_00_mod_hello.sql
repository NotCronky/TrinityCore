-- mod-hello: login counter per character
CREATE TABLE IF NOT EXISTS `mod_hello_logins` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `logins` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
