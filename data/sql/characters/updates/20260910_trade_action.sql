-- Conversational trade: machine-readable action attached to a
-- delivered chat line. Format: 'TRADE|Item Name|count'. Read by
-- C++ delivery, which drives the mod-playerbots trade command
-- machinery when the line is spoken. Idempotent.
SET @stmt = (
    SELECT IF(
        COUNT(*) = 0,
        'ALTER TABLE `llm_chatter_messages` ADD COLUMN `action` VARCHAR(255) DEFAULT NULL AFTER `emote`',
        'SELECT 1'
    )
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'llm_chatter_messages'
      AND COLUMN_NAME = 'action'
);
PREPARE stmt FROM @stmt;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
