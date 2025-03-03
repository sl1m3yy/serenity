/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/Math.h>
#include <LibGfx/Gamma.h>
#include <LibGfx/Line.h>
#include <LibWeb/CSS/StyleValue.h>
#include <LibWeb/Painting/GradientPainting.h>

namespace Web::Painting {

static float normalized_gradient_angle_radians(float gradient_angle)
{
    // Adjust angle so 0 degrees is bottom
    float real_angle = 90 - gradient_angle;
    return real_angle * (AK::Pi<float> / 180);
}

static float calulate_gradient_length(Gfx::IntSize const& gradient_size, float sin_angle, float cos_angle)
{
    return AK::fabs(gradient_size.height() * sin_angle) + AK::fabs(gradient_size.width() * cos_angle);
}

static float calulate_gradient_length(Gfx::IntSize const& gradient_size, float gradient_angle)
{
    float angle = normalized_gradient_angle_radians(gradient_angle);
    float sin_angle, cos_angle;
    AK::sincos(angle, sin_angle, cos_angle);
    return calulate_gradient_length(gradient_size, sin_angle, cos_angle);
}

static ColorStopList resolve_color_stop_positions(auto const& color_stop_list, auto resolve_position_to_float)
{
    VERIFY(color_stop_list.size() >= 2);
    ColorStopList resolved_color_stops;

    auto color_stop_length = [&](auto& stop) {
        return stop.color_stop.second_position.has_value() ? 2 : 1;
    };

    size_t expanded_size = 0;
    for (auto& stop : color_stop_list)
        expanded_size += color_stop_length(stop);

    resolved_color_stops.ensure_capacity(expanded_size);
    for (auto& stop : color_stop_list) {
        auto resolved_stop = ColorStop { .color = stop.color_stop.color };
        for (int i = 0; i < color_stop_length(stop); i++)
            resolved_color_stops.append(resolved_stop);
    }

    // 1. If the first color stop does not have a position, set its position to 0%.
    resolved_color_stops.first().position = 0;
    //    If the last color stop does not have a position, set its position to 100%
    resolved_color_stops.last().position = 1.0f;

    // 2. If a color stop or transition hint has a position that is less than the
    //    specified position of any color stop or transition hint before it in the list,
    //    set its position to be equal to the largest specified position of any color stop
    //    or transition hint before it.
    auto max_previous_color_stop_or_hint = resolved_color_stops[0].position;
    auto resolve_stop_position = [&](auto& position) {
        float value = resolve_position_to_float(position);
        value = max(value, max_previous_color_stop_or_hint);
        max_previous_color_stop_or_hint = value;
        return value;
    };
    // Move this step somewhere generic (since I think this code can be mostly reused for conic gradients)
    size_t resolved_index = 0;
    for (auto& stop : color_stop_list) {
        if (stop.transition_hint.has_value())
            resolved_color_stops[resolved_index].transition_hint = resolve_stop_position(stop.transition_hint->value);
        if (stop.color_stop.position.has_value())
            resolved_color_stops[resolved_index].position = resolve_stop_position(*stop.color_stop.position);
        if (stop.color_stop.second_position.has_value())
            resolved_color_stops[++resolved_index].position = resolve_stop_position(*stop.color_stop.second_position);
        ++resolved_index;
    }

    // 3. If any color stop still does not have a position, then, for each run of adjacent color stops
    //    without positions, set their positions so that they are evenly spaced between the preceding
    //    and following color stops with positions.
    // Note: Though not mentioned anywhere in the specification transition hints are counted as "color stops with positions".
    size_t i = 1;
    auto find_run_end = [&] {
        auto color_stop_has_position = [](auto& color_stop) {
            return color_stop.transition_hint.has_value() || isfinite(color_stop.position);
        };
        while (i < color_stop_list.size() - 1 && !color_stop_has_position(resolved_color_stops[i])) {
            i++;
        }
        return i;
    };
    while (i < resolved_color_stops.size() - 1) {
        auto& stop = resolved_color_stops[i];
        if (!isfinite(stop.position)) {
            auto run_start = i - 1;
            auto start_position = resolved_color_stops[i++].transition_hint.value_or(resolved_color_stops[run_start].position);
            auto run_end = find_run_end();
            auto end_position = resolved_color_stops[run_end].transition_hint.value_or(resolved_color_stops[run_end].position);
            auto spacing = (end_position - start_position) / (run_end - run_start);
            for (auto j = run_start + 1; j < run_end; j++) {
                resolved_color_stops[j].position = start_position + (j - run_start) * spacing;
            }
        }
        i++;
    }

    // Determine the location of the transition hint as a percentage of the distance between the two color stops,
    // denoted as a number between 0 and 1, where 0 indicates the hint is placed right on the first color stop,
    // and 1 indicates the hint is placed right on the second color stop.
    for (size_t i = 1; i < resolved_color_stops.size(); i++) {
        auto& color_stop = resolved_color_stops[i];
        auto& previous_color_stop = resolved_color_stops[i - 1];
        if (color_stop.transition_hint.has_value()) {
            auto stop_length = color_stop.position - previous_color_stop.position;
            color_stop.transition_hint = stop_length > 0 ? (*color_stop.transition_hint - previous_color_stop.position) / stop_length : 0;
        }
    }

    return resolved_color_stops;
}

LinearGradientData resolve_linear_gradient_data(Layout::Node const& node, Gfx::FloatSize const& gradient_size, CSS::LinearGradientStyleValue const& linear_gradient)
{
    auto gradient_angle = linear_gradient.angle_degrees(gradient_size);
    auto gradient_length_px = calulate_gradient_length(gradient_size.to_rounded<int>(), gradient_angle);
    auto gradient_length = CSS::Length::make_px(gradient_length_px);

    auto resolved_color_stops = resolve_color_stop_positions(linear_gradient.color_stop_list(), [&](auto const& length_percentage) {
        return length_percentage.resolved(node, gradient_length).to_px(node) / gradient_length_px;
    });

    Optional<float> repeat_length = {};
    if (linear_gradient.is_repeating())
        repeat_length = resolved_color_stops.last().position - resolved_color_stops.first().position;

    return { gradient_angle, resolved_color_stops, repeat_length };
}

ConicGradientData resolve_conic_gradient_data(Layout::Node const& node, CSS::ConicGradientStyleValue const& conic_gradient)
{
    CSS::Angle one_turn(360.0f, CSS::Angle::Type::Deg);
    auto resolved_color_stops = resolve_color_stop_positions(conic_gradient.color_stop_list(), [&](auto const& angle_percentage) {
        return angle_percentage.resolved(node, one_turn).to_degrees() / one_turn.to_degrees();
    });
    return { conic_gradient.angle_degrees(), resolved_color_stops };
}

static float color_stop_step(ColorStop const& previous_stop, ColorStop const& next_stop, float position)
{
    if (position < previous_stop.position)
        return 0;
    if (position > next_stop.position)
        return 1;
    // For any given point between the two color stops,
    // determine the point’s location as a percentage of the distance between the two color stops.
    // Let this percentage be P.
    auto stop_length = next_stop.position - previous_stop.position;
    // FIXME: Avoids NaNs... Still not quite correct?
    if (stop_length <= 0)
        return 1;
    auto p = (position - previous_stop.position) / stop_length;
    if (!next_stop.transition_hint.has_value())
        return p;
    if (*next_stop.transition_hint >= 1)
        return 0;
    if (*next_stop.transition_hint <= 0)
        return 1;
    // Let C, the color weighting at that point, be equal to P^(logH(.5)).
    auto c = AK::pow(p, AK::log<float>(0.5) / AK::log(*next_stop.transition_hint));
    // The color at that point is then a linear blend between the colors of the two color stops,
    // blending (1 - C) of the first stop and C of the second stop.
    return c;
}

class GradientLine {
public:
    GradientLine(int color_count, Span<ColorStop const> color_stops)
        : GradientLine(color_count, color_count, 0, false, color_stops) {};

    GradientLine(int color_count, int gradient_length, int start_offset, bool repeating, Span<ColorStop const> color_stops)
        : m_start_offset { start_offset }
        , m_repeating { repeating }
    {
        // Note: color_count will be < gradient_length for repeating gradients.
        m_gradient_line_colors.resize(color_count);
        // Note: color.mixed_with() performs premultiplied alpha mixing when necessary as defined in:
        // https://drafts.csswg.org/css-images/#coloring-gradient-line
        for (int loc = 0; loc < color_count; loc++) {
            auto relative_loc = float(loc + start_offset) / gradient_length;
            Gfx::Color gradient_color = color_stops[0].color.mixed_with(
                color_stops[1].color,
                color_stop_step(color_stops[0], color_stops[1], relative_loc));
            for (size_t i = 1; i < color_stops.size() - 1; i++) {
                gradient_color = gradient_color.mixed_with(
                    color_stops[i + 1].color,
                    color_stop_step(color_stops[i], color_stops[i + 1], relative_loc));
            }
            m_gradient_line_colors[loc] = gradient_color;
        }
    }

    Gfx::Color get_color(int index) const
    {
        return m_gradient_line_colors[clamp(index, 0, m_gradient_line_colors.size() - 1)];
    }

    Gfx::Color sample_color(float loc) const
    {
        auto repeat_wrap_if_required = [&](int loc) {
            if (m_repeating)
                return (loc + m_start_offset) % static_cast<int>(m_gradient_line_colors.size());
            return loc;
        };
        auto int_loc = static_cast<int>(loc);
        auto blend = loc - int_loc;
        // Blend between the two neighbouring colors (this fixes some nasty aliasing issues at small angles)
        return get_color(repeat_wrap_if_required(int_loc)).mixed_with(get_color(repeat_wrap_if_required(int_loc + 1)), blend);
    }

    void paint_into_rect(Gfx::Painter& painter, Gfx::IntRect const& rect, auto location_transform)
    {
        for (int y = 0; y < rect.height(); y++) {
            for (int x = 0; x < rect.width(); x++) {
                auto gradient_color = sample_color(location_transform(x, y));
                painter.set_pixel(rect.x() + x, rect.y() + y, gradient_color, gradient_color.alpha() < 255);
            }
        }
    }

private:
    int m_start_offset;
    bool m_repeating;
    Vector<Gfx::Color, 1024> m_gradient_line_colors;
};

void paint_linear_gradient(PaintContext& context, Gfx::IntRect const& gradient_rect, LinearGradientData const& data)
{
    float angle = normalized_gradient_angle_radians(data.gradient_angle);
    float sin_angle, cos_angle;
    AK::sincos(angle, sin_angle, cos_angle);

    // Full length of the gradient
    auto gradient_length_px = round_to<int>(calulate_gradient_length(gradient_rect.size(), sin_angle, cos_angle));

    Gfx::FloatPoint offset { cos_angle * (gradient_length_px / 2), sin_angle * (gradient_length_px / 2) };

    auto center = gradient_rect.translated(-gradient_rect.location()).center();
    auto start_point = center.to_type<float>() - offset;

    // Rotate gradient line to be horizontal
    auto rotated_start_point_x = start_point.x() * cos_angle - start_point.y() * -sin_angle;

    bool repeating = data.repeat_length.has_value();
    auto gradient_color_count = round_to<int>(data.repeat_length.value_or(1.0f) * gradient_length_px);
    auto& color_stops = data.color_stops;
    auto start_offset = repeating ? color_stops.first().position : 0.0f;
    auto start_offset_px = round_to<int>(start_offset * gradient_length_px);

    GradientLine gradient_line(gradient_color_count, gradient_length_px, start_offset_px, repeating, color_stops);
    gradient_line.paint_into_rect(context.painter(), gradient_rect, [&](int x, int y) {
        return (x * cos_angle - (gradient_rect.height() - y) * -sin_angle) - rotated_start_point_x;
    });
}

void paint_conic_gradient(PaintContext& context, Gfx::IntRect const& gradient_rect, ConicGradientData const& data, Gfx::IntPoint position)
{
    // FIXME: Do we need/want sub-degree accuracy for the gradient line?
    GradientLine gradient_line(360, data.color_stops);
    float start_angle = (360.0f - data.start_angle) + 90.0f;
    // Translate position/center to the center of the pixel (avoids some funky painting)
    auto center_point = Gfx::FloatPoint { position }.translated(0.5, 0.5);
    gradient_line.paint_into_rect(context.painter(), gradient_rect, [&](int x, int y) {
        auto point = Gfx::FloatPoint { x, y } - center_point;
        // FIXME: We could probably get away with some approximation here:
        // Note: We need too floor the angle here or the colors will start to diverge as you get further from the center.
        return floor(fmod((AK::atan2(point.y(), point.x()) * 180.0f / AK::Pi<float> + 360.0f + start_angle), 360.0f));
    });
}

}
