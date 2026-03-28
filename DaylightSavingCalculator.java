import java.time.*;
import java.time.format.DateTimeFormatter;
import java.time.format.DateTimeParseException;
import java.time.zone.ZoneOffsetTransition;
import java.time.zone.ZoneRules;

/**
 * Calculates how many sleeps until the next daylight saving time change.
 *
 * Usage:
 *   java DaylightSavingCalculator [date] [timezone]
 *
 *   date     - optional, format YYYY-MM-DD (default: today)
 *   timezone - optional, e.g. Europe/London, America/New_York (default: system timezone)
 *
 * Examples:
 *   java DaylightSavingCalculator
 *   java DaylightSavingCalculator 2026-03-01
 *   java DaylightSavingCalculator 2026-03-01 Europe/London
 */
public class DaylightSavingCalculator {

    public static void main(String[] args) {
        LocalDate fromDate;
        ZoneId zoneId;

        // Parse arguments
        try {
            fromDate = parseDate(args);
            zoneId = parseZone(args);
        } catch (IllegalArgumentException e) {
            System.err.println("Error: " + e.getMessage());
            printUsage();
            System.exit(1);
            return;
        }

        ZoneRules rules = zoneId.getRules();

        if (!rules.isDaylightSavings(Instant.now()) && rules.getTransitions().isEmpty()
                && rules.getTransitionRules().isEmpty()) {
            System.out.printf("Timezone %s does not observe daylight saving time.%n", zoneId);
            System.exit(0);
            return;
        }

        // Find the next DST transition from the start of the given date in the target zone
        ZonedDateTime fromDateTime = fromDate.atStartOfDay(zoneId);
        ZoneOffsetTransition nextTransition = rules.nextTransition(fromDateTime.toInstant());

        if (nextTransition == null) {
            System.out.printf("No future DST transitions found for timezone %s.%n", zoneId);
            System.exit(0);
            return;
        }

        // Calculate sleeps: number of midnights between fromDate and the transition date
        LocalDate transitionDate = nextTransition.getDateTimeBefore().toLocalDate();
        long sleeps = fromDate.until(transitionDate, java.time.temporal.ChronoUnit.DAYS);

        // Determine if clocks go forward (summer/spring) or back (winter/autumn)
        boolean isSpringForward = nextTransition.getOffsetAfter().getTotalSeconds()
                > nextTransition.getOffsetBefore().getTotalSeconds();
        String changeType = isSpringForward ? "summer time (clocks spring forward)" : "winter time (clocks fall back)";

        DateTimeFormatter dateFormatter = DateTimeFormatter.ofPattern("EEEE, d MMMM yyyy");
        String transitionDateFormatted = transitionDate.format(dateFormatter);

        System.out.println("----------------------------------------------------");
        System.out.printf("From date : %s%n", fromDate.format(dateFormatter));
        System.out.printf("Timezone  : %s%n", zoneId);
        System.out.println("----------------------------------------------------");
        System.out.printf("Next DST change : %s%n", transitionDateFormatted);
        System.out.printf("Change type     : %s%n", changeType);
        System.out.printf("Change time     : %s -> %s%n",
                nextTransition.getDateTimeBefore().toLocalTime(),
                nextTransition.getDateTimeAfter().toLocalTime());

        if (sleeps == 0) {
            System.out.println("Sleeps till change: today!");
        } else if (sleeps == 1) {
            System.out.println("Sleeps till change: 1 sleep to go!");
        } else {
            System.out.printf("Sleeps till change: %d sleeps to go!%n", sleeps);
        }
        System.out.println("----------------------------------------------------");
    }

    private static LocalDate parseDate(String[] args) {
        if (args.length >= 1) {
            try {
                return LocalDate.parse(args[0], DateTimeFormatter.ISO_LOCAL_DATE);
            } catch (DateTimeParseException e) {
                throw new IllegalArgumentException(
                        "Invalid date format '" + args[0] + "'. Expected YYYY-MM-DD.");
            }
        }
        return LocalDate.now();
    }

    private static ZoneId parseZone(String[] args) {
        if (args.length >= 2) {
            try {
                return ZoneId.of(args[1]);
            } catch (Exception e) {
                throw new IllegalArgumentException(
                        "Unknown timezone '" + args[1] + "'. Example: Europe/London, America/New_York");
            }
        }
        return ZoneId.systemDefault();
    }

    private static void printUsage() {
        System.out.println();
        System.out.println("Usage: java DaylightSavingCalculator [date] [timezone]");
        System.out.println();
        System.out.println("  date     - optional, format YYYY-MM-DD (default: today)");
        System.out.println("  timezone - optional, e.g. Europe/London, America/New_York");
        System.out.println("             (default: system timezone)");
        System.out.println();
        System.out.println("Examples:");
        System.out.println("  java DaylightSavingCalculator");
        System.out.println("  java DaylightSavingCalculator 2026-03-01");
        System.out.println("  java DaylightSavingCalculator 2026-03-01 Europe/London");
        System.out.println("  java DaylightSavingCalculator 2026-10-15 America/New_York");
    }
}
