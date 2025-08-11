/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   Logger.hpp                                         :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: vvelikov <vvelikov@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2025/08/08 22:09:33 by vvelikov          #+#    #+#             */
/*   Updated: 2025/08/08 22:12:43 by vvelikov         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <iostream>
#include <string>

#define LOG_INFO(msg) (std::cout << "[INFO] " << msg << std::endl)
#define LOG_ERROR(msg) (std::cerr << "[ERROR] " << msg << std::endl)
#define LOG_DEBUG(msg) (std::cout << "[DEBUG] " << msg << std::endl)




#endif