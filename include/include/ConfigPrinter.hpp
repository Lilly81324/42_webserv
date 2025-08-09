/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   ConfigPrinter.hpp                                  :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: vvelikov <vvelikov@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2025/08/09 17:11:40 by vvelikov          #+#    #+#             */
/*   Updated: 2025/08/09 17:30:52 by vvelikov         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef CONFIG_PRINTER_HPP
#define CONFIG_PRINTER_HPP

#include <iosfwd>
#include "Config.hpp"

void printConfigSummary(std::ostream& os, const Config& cfg);

#endif
