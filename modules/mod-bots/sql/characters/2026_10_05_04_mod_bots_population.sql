-- mod-bots: the accounts and characters the bot population manager created.

CREATE TABLE IF NOT EXISTS `bot_accounts` (
  `id` INT UNSIGNED NOT NULL COMMENT 'auth.account.id',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `bot_characters` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `account` INT UNSIGNED NOT NULL COMMENT 'bot_accounts.id',
  PRIMARY KEY (`guid`),
  KEY `account` (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
