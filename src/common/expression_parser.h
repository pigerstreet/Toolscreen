#pragma once


#include <string>
#include <cmath>

int EvaluateExpression(const std::string& expr, int screenWidth, int screenHeight, int defaultValue = 0);

bool IsExpression(const std::string& str);

bool ValidateExpression(const std::string& expr, std::string& errorOut);

void RecalculateExpressionDimensions();


